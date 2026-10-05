/* Transliterated from the test blocks in Ghostty src/terminal/Screen.zig
 * and Selection.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 */

#include "vt_screen_test_helpers.hpp"

TEST(screen, Screen_forwards_optional_scrollback_limits) {
    const size_t max_lines = 123;
    SCREEN_OPTS(s, Screen::Options(80, 24, Maybe<size_t>(), max_lines));
    ASSERT_TRUE(SIZE_MAX == s.pages.limits.bytes.explicit_);
    ASSERT_TRUE(max_lines == s.pages.limits.lines.explicit_);
    ASSERT_TRUE(!s.no_scrollback);
}

TEST(screen, Screen_reset_cursor_pin_is_not_garbage) {
    ScreenHolder h(Screen::Options(80, 24, (size_t)1000));
    ASSERT_TRUE(h.ok);
    Screen &s = h.s;
    ASSERT_TRUE(s.testWriteString("hello, world"));
    s.reset();
    ASSERT_TRUE(!s.cursor.page_pin->garbage);
}

TEST(screen, Screen_read_and_write) {
    ScreenHolder h(Screen::Options(80, 24, (size_t)1000));
    ASSERT_TRUE(h.ok);
    Screen &s = h.s;
    ASSERT_TRUE(s.cursor.style_id == 0);
    ASSERT_TRUE(s.testWriteString("hello, world"));
    ASSERT_TRUE(s.dumpStringAlloc(Point::screen()) == "hello, world");
}

TEST(screen, Screen_read_and_write_newline) {
    ScreenHolder h(Screen::Options(80, 24, (size_t)1000));
    ASSERT_TRUE(h.ok);
    Screen &s = h.s;
    ASSERT_TRUE(s.testWriteString("hello\nworld"));
    ASSERT_TRUE(s.dumpStringAlloc(Point::screen()) == "hello\nworld");
}

TEST(screen, Screen_read_and_write_scrollback) {
    ScreenHolder h(Screen::Options(80, 2, (size_t)1000));
    ASSERT_TRUE(h.ok);
    Screen &s = h.s;
    ASSERT_TRUE(s.testWriteString("hello\nworld\ntest"));
    ASSERT_TRUE(s.dumpStringAlloc(Point::screen()) == "hello\nworld\ntest");
    ASSERT_TRUE(s.dumpStringAlloc(Point::active()) == "world\ntest");
}

TEST(screen, Screen_read_and_write_no_scrollback_small) {
    ScreenHolder h(Screen::Options(80, 2, (size_t)0));
    ASSERT_TRUE(h.ok);
    Screen &s = h.s;
    ASSERT_TRUE(s.testWriteString("hello\nworld\ntest"));
    ASSERT_TRUE(s.dumpStringAlloc(Point::screen()) == "world\ntest");
    ASSERT_TRUE(s.dumpStringAlloc(Point::active()) == "world\ntest");
}

TEST(screen, Screen_read_and_write_no_scrollback_large) {
    ScreenHolder h(Screen::Options(80, 2, (size_t)0));
    ASSERT_TRUE(h.ok);
    Screen &s = h.s;
    for (int i = 0; i < 1000; i++) {
        char buf[128];
        snprintf(buf, sizeof buf, "%d\n", i);
        ASSERT_TRUE(s.testWriteString(buf));
    }
    ASSERT_TRUE(s.testWriteString("1000"));
    ASSERT_TRUE(s.dumpStringAlloc(Point::screen()) == "999\n1000");
}

TEST(screen, Screen_cursorCopy_x_y) {
    SCREEN(s, 10, 10, (size_t)0);
    s.cursorAbsolute(2, 3);
    ASSERT_TRUE(s.cursor.x == 2);
    ASSERT_TRUE(s.cursor.y == 3);

    SCREEN(s2, 10, 10, (size_t)0);
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(s2.cursor.x == 2);
    ASSERT_TRUE(s2.cursor.y == 3);
    WRITE(s2, "Hello");

    EXPECT_DUMP(s2, screen, "\n\n\n  Hello");
}

TEST(screen, Screen_cursorCopy_style_deref) {
    SCREEN(s, 10, 10, (size_t)0);

    SCREEN(s2, 10, 10, (size_t)0);
    Page *page = s2.cursor.page_pin->node->page();

    /* Bold should create our style */
    NOERR(s2.setAttribute(attr(A::bold)));
    ASSERT_TRUE(1 == page->styles.count());
    ASSERT_TRUE(s2.cursor.style.flags.bold);

    /* Copy default style, should release our style */
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(!s2.cursor.style.flags.bold);
    ASSERT_TRUE(0 == page->styles.count());
}

/* Wisp: shared setup of the "new page" cursorCopy tests. Fill the
 * scrollback with blank lines until there are only 5 rows left on the
 * first page, then write 10 lines. */
static bool fillToSecondPage(Screen &s2) {
    const size_t first_page_size = s2.pages.pages.first->capacity().rows;
    s2.pages.pages.first->page()->pauseIntegrityChecks(true);
    for (size_t i = 0; i < first_page_size - 5; i++) {
        if (!s2.testWriteString("\n")) return false;
    }
    s2.pages.pages.first->page()->pauseIntegrityChecks(false);
    return s2.testWriteString("1\n2\n3\n4\n5\n6\n7\n8\n9\n10");
}

TEST(screen, Screen_cursorCopy_style_deref_new_page) {
    SCREEN(s, 10, 10, (size_t)0);
    SCREEN(s2, 10, 10, (size_t)2048);

    /* We need to get the cursor on a new page. */
    ASSERT_TRUE(fillToSecondPage(s2));

    /* This should be PAGE 1 */
    Page *page = s2.cursor.page_pin->node->page();

    /* It should be the last page in the list. */
    ASSERT_TRUE(s2.pages.pages.last->page() == page);
    /* It should have a previous page. */
    ASSERT_TRUE(s2.cursor.page_pin->node->prev != nullptr);

    /* The cursor should be at 2, 9 */
    ASSERT_TRUE(s2.cursor.x == 2);
    ASSERT_TRUE(s2.cursor.y == 9);

    /* Bold should create our style in page 1. */
    NOERR(s2.setAttribute(attr(A::bold)));
    ASSERT_TRUE(1 == page->styles.count());
    ASSERT_TRUE(s2.cursor.style.flags.bold);

    /* Copy the cursor for the first screen. This should release
     * the style from page 1 and move the cursor back to page 0. */
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(!s2.cursor.style.flags.bold);
    ASSERT_TRUE(0 == page->styles.count());
    /* The page after the page the cursor is now in should be page 1. */
    ASSERT_TRUE(page == s2.cursor.page_pin->node->next->page());
    /* The cursor should be at 0, 0 */
    ASSERT_TRUE(s2.cursor.x == 0);
    ASSERT_TRUE(s2.cursor.y == 0);
}

TEST(screen, Screen_cursorCopy_style_copy) {
    SCREEN(s, 10, 10, (size_t)0);
    NOERR(s.setAttribute(attr(A::bold)));

    SCREEN(s2, 10, 10, (size_t)0);
    Page *page = s2.cursor.page_pin->node->page();
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(s2.cursor.style.flags.bold);
    ASSERT_TRUE(1 == page->styles.count());
}

TEST(screen, Screen_cursorCopy_hyperlink_deref) {
    SCREEN(s, 10, 10, (size_t)0);

    SCREEN(s2, 10, 10, (size_t)0);
    Page *page = s2.cursor.page_pin->node->page();

    /* Create a hyperlink for the cursor. */
    NOERR(startLink(s2, "https://example.com/"));
    ASSERT_TRUE(1 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id != 0);

    /* Copy a cursor with no hyperlink, should release our hyperlink. */
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(0 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id == 0);
}

TEST(screen, Screen_write_regrows_compacted_page_capacity) {
    SCREEN(s, 80, 24, (size_t)0);

    /* Compact the active page so every managed capacity dimension is
     * zero, then reload the cursor since its cached row/cell pointers
     * point into the replaced page. */
    {
        PageList::Node *node = nullptr;
        ASSERT_TRUE(s.pages.compact(s.cursor.page_pin->node, &node));
        ASSERT_TRUE(node != nullptr);
        ASSERT_TRUE(0 == node->capacity().styles);
        ASSERT_TRUE(0 == node->capacity().grapheme_bytes);
        ASSERT_TRUE(0 == node->capacity().string_bytes);
        ASSERT_TRUE(0 == node->capacity().hyperlink_bytes);
        s.cursorReload();
    }

    /* Styled write: exercises the manualStyleUpdate single-retry
     * path. Prior to increaseCapacity handling zero dimensions, the
     * retry would fail and the style would be dropped. */
    NOERR(s.setAttribute(attr(A::bold)));
    WRITE(s, "A");

    /* Grapheme write: exercises the appendGrapheme single-retry path.
     * We can't use testWriteString here because it appends graphemes
     * directly on the page without the capacity retry. */
    WRITE(s, "a");
    NOERR(s.appendGrapheme(s.cursorCellLeft(1), 0x0301));

    /* Hyperlink: exercises the startHyperlink retry loop, which used
     * to loop forever when capacity growth from zero didn't grow. */
    NOERR(startLink(s, "https://example.com/"));
    WRITE(s, "B");
    s.endHyperlink();

    /* Verify the content landed on the page. */
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(page->styles.count() >= 1);
    ASSERT_TRUE(page->hyperlink_set.count() >= 1);
    ASSERT_TRUE(page->graphemeCount() >= 1);
}

/* The cursor style and hyperlink IDs are only meaningful within the page
 * the cursor pin points at. scrollClear can move the active area onto a
 * later page while the cursor pin stays with its content on an earlier
 * page (now scrollback), so the reset in cursorReload must migrate both
 * references to the destination page. It previously replaced the pin
 * directly and then released the old style ID on the new page. */
TEST(screen, Screen_scrollClear_across_pages_migrates_cursor_style_and_hyperlink) {
    SCREEN(s, 10, 10, unlimited);

    /* Fill the first page so the active area spans two pages. */
    ASSERT_TRUE(fillToSecondPage(s));
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);

    /* Move the cursor to the top of the active area, which is on the
     * first page, and give it a style and a hyperlink there. */
    s.cursorAbsolute(0, 0);
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);
    NOERR(s.setAttribute(attr(A::bold)));
    NOERR(startLink(s, "https://example.com/"));

    Page *old_page = s.cursor.page_pin->node->page();
    const style::Id old_style_id = s.cursor.style_id;
    const hyperlink::Id old_hyperlink_id = s.cursor.hyperlink_id;
    ASSERT_TRUE(old_style_id != style::default_id);
    ASSERT_TRUE(old_hyperlink_id != 0);

    /* All ten active rows are non-empty, so this moves the active area
     * fully onto the second page while the cursor pin stays with its
     * old row, which is now scrollback. */
    ASSERT_TRUE(s.scrollClear());

    /* The cursor was moved to the new active top-left on the second
     * page with its style and hyperlink references rebuilt there. */
    Page *new_page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(new_page != old_page);
    ASSERT_TRUE(s.cursor.style_id != style::default_id);
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    ASSERT_TRUE(new_page->styles.refCount((const void *)new_page->memory, s.cursor.style_id) > 0);
    ASSERT_TRUE(new_page->hyperlink_set.refCount((const void *)new_page->memory, s.cursor.hyperlink_id) > 0);

    /* The cursor's references on the old page were released. Nothing
     * else referenced either entry, so both are dead there now. */
    ASSERT_TRUE(0 == old_page->styles.refCount((const void *)old_page->memory, old_style_id));
    ASSERT_TRUE(0 == old_page->hyperlink_set.refCount((const void *)old_page->memory, old_hyperlink_id));

    /* Printing attaches the migrated style and hyperlink to a cell. */
    WRITE(s, "B");
}

TEST(screen, Screen_cursorCopy_hyperlink_deref_new_page) {
    SCREEN(s, 10, 10, (size_t)0);
    SCREEN(s2, 10, 10, (size_t)2048);

    /* We need to get the cursor on a new page. */
    ASSERT_TRUE(fillToSecondPage(s2));

    /* This should be PAGE 1 */
    Page *page = s2.cursor.page_pin->node->page();

    /* It should be the last page in the list. */
    ASSERT_TRUE(s2.pages.pages.last->page() == page);
    /* It should have a previous page. */
    ASSERT_TRUE(s2.cursor.page_pin->node->prev != nullptr);

    /* The cursor should be at 2, 9 */
    ASSERT_TRUE(s2.cursor.x == 2);
    ASSERT_TRUE(s2.cursor.y == 9);

    /* Create a hyperlink for the cursor, should be in page 1. */
    NOERR(startLink(s2, "https://example.com/"));
    ASSERT_TRUE(1 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id != 0);

    /* Copy the cursor for the first screen. This should release
     * the hyperlink from page 1 and move the cursor back to page 0. */
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(0 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id == 0);
    /* The page after the page the cursor is now in should be page 1. */
    ASSERT_TRUE(page == s2.cursor.page_pin->node->next->page());
    /* The cursor should be at 0, 0 */
    ASSERT_TRUE(s2.cursor.x == 0);
    ASSERT_TRUE(s2.cursor.y == 0);
}

TEST(screen, Screen_cursorCopy_hyperlink_copy) {
    SCREEN(s, 10, 10, (size_t)0);

    /* Create a hyperlink for the cursor. */
    NOERR(startLink(s, "https://example.com/"));
    ASSERT_TRUE(1 == s.cursor.page_pin->node->page()->hyperlink_set.count());
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);

    SCREEN(s2, 10, 10, (size_t)0);
    Page *page = s2.cursor.page_pin->node->page();

    ASSERT_TRUE(0 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id == 0);

    /* Copy the cursor with the hyperlink. */
    NOERR(s2.cursorCopy(s.cursor));
    ASSERT_TRUE(1 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id != 0);
}

TEST(screen, Screen_cursorCopy_hyperlink_copy_disabled) {
    SCREEN(s, 10, 10, (size_t)0);

    /* Create a hyperlink for the cursor. */
    NOERR(startLink(s, "https://example.com/"));
    ASSERT_TRUE(1 == s.cursor.page_pin->node->page()->hyperlink_set.count());
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);

    SCREEN(s2, 10, 10, (size_t)0);
    Page *page = s2.cursor.page_pin->node->page();

    ASSERT_TRUE(0 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id == 0);

    /* Copy the cursor with the hyperlink. */
    NOERR(s2.cursorCopy(s.cursor, false));
    ASSERT_TRUE(0 == page->hyperlink_set.count());
    ASSERT_TRUE(s2.cursor.hyperlink_id == 0);
}

TEST(screen, Screen_style_basics) {
    SCREEN(s, 80, 24, (size_t)1000);
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(0 == page->styles.count());

    /* Set a new style */
    NOERR(s.setAttribute(attr(A::bold)));
    ASSERT_TRUE(s.cursor.style_id != 0);
    ASSERT_TRUE(1 == page->styles.count());
    ASSERT_TRUE(s.cursor.style.flags.bold);

    /* Set another style, we should still only have one since it was unused */
    NOERR(s.setAttribute(attr(A::italic)));
    ASSERT_TRUE(s.cursor.style_id != 0);
    ASSERT_TRUE(1 == page->styles.count());
    ASSERT_TRUE(s.cursor.style.flags.italic);
}

TEST(screen, Screen_style_reset_to_default) {
    SCREEN(s, 80, 24, (size_t)1000);
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(0 == page->styles.count());

    /* Set a new style */
    NOERR(s.setAttribute(attr(A::bold)));
    ASSERT_TRUE(s.cursor.style_id != 0);
    ASSERT_TRUE(1 == page->styles.count());

    /* Reset to default */
    NOERR(s.setAttribute(attr(A::reset_bold)));
    ASSERT_TRUE(s.cursor.style_id == 0);
    ASSERT_TRUE(0 == page->styles.count());
}

TEST(screen, Screen_style_reset_with_unset) {
    SCREEN(s, 80, 24, (size_t)1000);
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(0 == page->styles.count());

    /* Set a new style */
    NOERR(s.setAttribute(attr(A::bold)));
    ASSERT_TRUE(s.cursor.style_id != 0);
    ASSERT_TRUE(1 == page->styles.count());

    /* Reset to default */
    NOERR(s.setAttribute(attr(A::unset)));
    ASSERT_TRUE(s.cursor.style_id == 0);
    ASSERT_TRUE(0 == page->styles.count());
}

TEST(screen, Screen_clearRows_active_one_line) {
    SCREEN(s, 80, 24, (size_t)1000);

    WRITE(s, "hello, world");
    s.clearRows(Point::active(), Maybe<Point>(), false);
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    EXPECT_DUMP(s, screen, "");
}

TEST(screen, Screen_clearRows_active_multi_line) {
    SCREEN(s, 80, 24, (size_t)1000);

    WRITE(s, "hello\nworld");
    s.clearRows(Point::active(), Maybe<Point>(), false);
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    EXPECT_DUMP(s, screen, "");
}

TEST(screen, Screen_clearRows_active_styled_line) {
    SCREEN(s, 80, 24, (size_t)1000);

    NOERR(s.setAttribute(attr(A::bold)));
    WRITE(s, "hello world");
    NOERR(s.setAttribute(attr(A::unset)));

    /* We should have one style */
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(1 == page->styles.count());

    s.clearRows(Point::active(), Maybe<Point>(), false);

    /* We should have none because active cleared it */
    ASSERT_TRUE(0 == page->styles.count());

    EXPECT_DUMP(s, screen, "");
}

TEST(screen, Screen_clearCells_empty_range) {
    SCREEN_OPTS(s, Screen::Options::default_());

    Page *page = s.cursor.page_pin->node->page();
    Row *row = s.cursor.page_row;
    Cell *cells = page->getCells(row);
    s.clearCells(page, row, cells, 0);
}

TEST(screen, Screen_clearRows_uses_stored_page_width) {
    SCREEN(s, 2, 1, (size_t)0);

    WRITE(s, "AB");
    PageList::Node *node = s.pages.pages.first;
    s.pages.cols = 4;

    s.clearRows(Point::screen(), Maybe<Point>(), false);
    Page *page = node->page();
    const Cell *cells = page->getCells(&page->rows.ptr(page->memory)[0]);
    ASSERT_TRUE(2 == page->size.cols);
    for (size_t i = 0; i < page->size.cols; i++) ASSERT_TRUE(cells[i].isEmpty());
}

TEST(screen, Screen_clearRows_protected) {
    SCREEN(s, 80, 24, (size_t)1000);

    WRITE(s, "UNPROTECTED");
    s.cursor.protected_ = true;
    WRITE(s, "PROTECTED");
    s.cursor.protected_ = false;
    WRITE(s, "UNPROTECTED");
    WRITE(s, "\n");
    s.cursor.protected_ = true;
    WRITE(s, "PROTECTED");
    s.cursor.protected_ = false;
    WRITE(s, "UNPROTECTED");
    s.cursor.protected_ = true;
    WRITE(s, "PROTECTED");
    s.cursor.protected_ = false;

    s.clearRows(Point::active(), Maybe<Point>(), true);

    EXPECT_DUMP(s, screen, "           PROTECTED\nPROTECTED           PROTECTED");
}

TEST(screen, Screen_eraseRows_history) {
    SCREEN(s, 5, 5, (size_t)1000);

    WRITE(s, "1\n2\n3\n4\n5\n6");

    EXPECT_DUMP(s, active, "2\n3\n4\n5\n6");
    EXPECT_DUMP(s, screen, "1\n2\n3\n4\n5\n6");

    s.eraseHistory(Maybe<Point>());

    EXPECT_DUMP(s, active, "2\n3\n4\n5\n6");
    EXPECT_DUMP(s, screen, "2\n3\n4\n5\n6");
}

TEST(screen, Screen_eraseRows_history_with_more_lines) {
    SCREEN(s, 5, 5, (size_t)1000);

    WRITE(s, "A\nB\nC\n1\n2\n3\n4\n5\n6");

    EXPECT_DUMP(s, active, "2\n3\n4\n5\n6");
    EXPECT_DUMP(s, screen, "A\nB\nC\n1\n2\n3\n4\n5\n6");

    s.eraseHistory(Maybe<Point>());

    EXPECT_DUMP(s, active, "2\n3\n4\n5\n6");
    EXPECT_DUMP(s, screen, "2\n3\n4\n5\n6");
}

TEST(screen, Screen_eraseRows_active_partial) {
    SCREEN(s, 5, 5, (size_t)0);

    WRITE(s, "1\n2\n3");

    EXPECT_DUMP(s, active, "1\n2\n3");

    s.eraseActive(1);

    EXPECT_DUMP(s, active, "3");
    EXPECT_DUMP(s, screen, "3");
}

TEST(screen, Screen__cursorCellEndOfPrev_across_mixed_width_pages) {
    SCREEN(s, 4, 2, (size_t)0);

    WRITE(s, "ABCDE");
    PageList::Node *first = s.pages.pages.first;
    ASSERT_TRUE(s.pages.split(Pin(first, 1)) == PageList::SplitError::none);
    s.cursorReload();
    PageList::Node *second = first->next;
    first->page()->size.cols = 2;

    ASSERT_TRUE(second == s.cursor.page_pin->node);
    const Cell *expected = Pin(first, 0, 1).rowAndCell().cell;
    ASSERT_TRUE(expected == s.cursorCellEndOfPrev());
}

/* Wisp: shared setup of the "across pages" cursor tests. Scroll down
 * enough to go to another page. */
static bool scrollToNewPage(Screen &s, Page *start_page) {
    const size_t rem = start_page->capacity.rows;
    start_page->pauseIntegrityChecks(true);
    for (size_t i = 0; i < rem; i++)
        if (!s.cursorDownOrScroll()) return false;
    start_page->pauseIntegrityChecks(false);
    return true;
}

TEST(screen, Screen__cursorDown_across_pages_preserves_style) {
    SCREEN(s, 10, 3, (size_t)1);

    /* Scroll down enough to go to another page */
    Page *start_page = s.pages.pages.last->page();
    ASSERT_TRUE(scrollToNewPage(s, start_page));

    /* We need our page to change for this test o make sense. If this
     * assertion fails then the bug is in the test: we should be scrolling
     * above enough for a new page to show up. */
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(start_page != page);
    }

    /* Scroll back to the previous page */
    s.cursorUp(1);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(start_page == page);
    }

    /* Go back up, set a style */
    NOERR(s.setAttribute(attr(A::bold)));
    {
        Page *page = s.cursor.page_pin->node->page();
        const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }

    /* Go back down into the next page and we should have that style */
    s.cursorDown(1);
    {
        Page *page = s.cursor.page_pin->node->page();
        const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }
}

TEST(screen, Screen__cursorUp_across_pages_preserves_style) {
    SCREEN(s, 10, 3, (size_t)1);

    /* Scroll down enough to go to another page */
    Page *start_page = s.pages.pages.last->page();
    ASSERT_TRUE(scrollToNewPage(s, start_page));

    /* We need our page to change for this test o make sense. If this
     * assertion fails then the bug is in the test: we should be scrolling
     * above enough for a new page to show up. */
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(start_page != page);
    }

    /* Go back up, set a style */
    NOERR(s.setAttribute(attr(A::bold)));
    {
        Page *page = s.cursor.page_pin->node->page();
        const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }

    /* Go back down into the prev page and we should have that style */
    s.cursorUp(1);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(start_page == page);

        const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }
}

TEST(screen, Screen__cursorAbsolute_across_pages_preserves_style) {
    SCREEN(s, 10, 3, (size_t)1);

    /* Scroll down enough to go to another page */
    Page *start_page = s.pages.pages.last->page();
    ASSERT_TRUE(scrollToNewPage(s, start_page));

    /* We need our page to change for this test o make sense. If this
     * assertion fails then the bug is in the test: we should be scrolling
     * above enough for a new page to show up. */
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(start_page != page);
    }

    /* Go back up, set a style */
    NOERR(s.setAttribute(attr(A::bold)));
    {
        Page *page = s.cursor.page_pin->node->page();
        const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }

    /* Go back down into the prev page and we should have that style */
    s.cursorAbsolute(1, 1);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(start_page == page);

        const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }
}

TEST(screen, Screen__cursorAbsolute_to_page_with_insufficient_capacity) {
    /* This test checks for a very specific edge case
     * which previously resulted in memory corruption.
     *
     * The conditions for this edge case are as such:
     * - The cursor has an associated style or other managed memory.
     * - The cursor moves to a different page.
     * - The new page is at capacity and must have its capacity adjusted. */
    SCREEN(s, 10, 3, (size_t)1);

    /* Scroll down enough to go to another page */
    Page *start_page = s.pages.pages.last->page();
    ASSERT_TRUE(scrollToNewPage(s, start_page));

    Page *new_page = s.cursor.page_pin->node->page();

    /* We need our page to change for this test to make sense. If this
     * assertion fails then the bug is in the test: we should be scrolling
     * above enough for a new page to show up. */
    ASSERT_TRUE(start_page != new_page);

    /* Add styles to the start page until it reaches capacity. */
    {
        /* Pause integrity checks because they're slow and
         * we're not testing this, this is just setup. */
        start_page->pauseIntegrityChecks(true);

        uint32_t n = 1;
        for (;;) {
            style::Style st;
            st.bg_color = style::Style::Color::makeRgb(
                style::RGB((uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF), (uint8_t)((n >> 16) & 0xFF)));
            style::Id id;
            if (start_page->styles.add((const void *)start_page->memory, st, &id) != ref_counted_set::AddError::none)
                break;
            n += 1;
        }

        start_page->pauseIntegrityChecks(false);
        start_page->assertIntegrity();
    }

    /* Set a style on the cursor. */
    NOERR(s.setAttribute(attr(A::bold)));
    {
        const style::Style *styleval = new_page->styles.get((const void *)new_page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }

    /* Go back up into the start page and we should still have that style. */
    s.cursorAbsolute(1, 1);
    {
        Page *cur_page = s.cursor.page_pin->node->page();
        /* The page we're on now should NOT equal start_page, since its
         * capacity should have been adjusted, which invalidates our ptr. */
        ASSERT_TRUE(start_page != cur_page);
        /* To make sure we DID change pages we check we're not on new_page. */
        ASSERT_TRUE(new_page != cur_page);

        const style::Style *styleval = cur_page->styles.get((const void *)cur_page->memory, s.cursor.style_id);
        ASSERT_TRUE(styleval->flags.bold);
    }

    s.cursor.page_pin->node->page()->assertIntegrity();
    new_page->assertIntegrity();
}

TEST(screen, Screen__scrolling) {
    SCREEN(s, 10, 3, (size_t)0);
    NOERR(s.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 155, 0, 0)));
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Scroll down, should still be bottom */
    ASSERT_TRUE(s.cursorDownScroll());
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::active(0, 2)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        ASSERT_TRUE(rgbEq(cell->contentColorRgb(), 155, 0, 0));
    }

    /* Everything is dirty because we have no scrollback */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));

    /* Scrolling to the bottom does nothing */
    s.scroll(Screen::Scroll::active());

    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");
}

TEST(screen, Screen__scrolling_with_a_single_row_screen_no_scrollback) {
    SCREEN(s, 10, 1, (size_t)0);
    WRITE(s, "1ABCD");

    /* Scroll down, should still be bottom */
    ASSERT_TRUE(s.cursorDownScroll());
    EXPECT_DUMP(s, viewport, "");

    /* Screen should be dirty */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
}

TEST(screen, Screen__scrolling_with_a_single_row_screen_with_scrollback) {
    SCREEN(s, 10, 1, (size_t)1);
    WRITE(s, "1ABCD");

    /* Scroll down, should still be bottom */
    ASSERT_TRUE(s.cursorDownScroll());
    EXPECT_DUMP(s, viewport, "");

    /* Active should be dirty */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));

    /* Scrollback also dirty because cursor moved from there */
    ASSERT_TRUE(s.pages.isDirty(Point::screen(0, 0)));

    s.scroll(Screen::Scroll::deltaRow(-1));
    EXPECT_DUMP(s, viewport, "1ABCD");
}

TEST(screen, Screen__scrolling_across_pages_preserves_style) {
    SCREEN(s, 10, 3, (size_t)1);
    NOERR(s.setAttribute(attr(A::bold)));
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    Page *start_page = s.pages.pages.last->page();

    /* Scroll down enough to go to another page */
    const size_t rem = start_page->capacity.rows - start_page->size.rows + 1;
    start_page->pauseIntegrityChecks(true);
    for (size_t i = 0; i < rem; i++) ASSERT_TRUE(s.cursorDownOrScroll());
    start_page->pauseIntegrityChecks(false);

    /* We need our page to change for this test o make sense. If this
     * assertion fails then the bug is in the test: we should be scrolling
     * above enough for a new page to show up. */
    Page *page = s.pages.pages.last->page();
    ASSERT_TRUE(start_page != page);

    const style::Style *styleval = page->styles.get((const void *)page->memory, s.cursor.style_id);
    ASSERT_TRUE(styleval->flags.bold);
}

TEST(screen, Screen__scroll_down_from_0) {
    SCREEN(s, 10, 3, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Scrolling up does nothing, but allows it */
    s.scroll(Screen::Scroll::deltaRow(-1));
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);

    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");
}

TEST(screen, Screen__scrollback_various_cases) {
    SCREEN(s, 10, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    ASSERT_TRUE(s.cursorDownScroll());

    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");

    /* Scrolling to the bottom */
    s.scroll(Screen::Scroll::active());
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");

    /* Scrolling back should make it visible again */
    s.scroll(Screen::Scroll::deltaRow(-1));
    ASSERT_TRUE(s.pages.viewport != PageList::Viewport::active);
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Scrolling back again should do nothing */
    s.scroll(Screen::Scroll::deltaRow(-1));
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Scrolling to the bottom */
    s.scroll(Screen::Scroll::active());
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");

    /* Scrolling forward with no grow should do nothing */
    s.scroll(Screen::Scroll::deltaRow(1));
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");

    /* Scrolling to the top should work */
    s.scroll(Screen::Scroll::top());
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Should be able to easily clear active area only */
    s.clearRows(Point::active(), Maybe<Point>(), false);
    EXPECT_DUMP(s, viewport, "1ABCD");

    /* Scrolling to the bottom */
    s.scroll(Screen::Scroll::active());
    EXPECT_DUMP(s, viewport, "");
}

TEST(screen, Screen__scrollback_with_multi_row_delta) {
    SCREEN(s, 10, 3, (size_t)3);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH\n6IJKL");

    /* Scroll to top */
    s.scroll(Screen::Scroll::top());
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Scroll down multiple */
    s.scroll(Screen::Scroll::deltaRow(5));
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);
    EXPECT_DUMP(s, viewport, "4ABCD\n5EFGH\n6IJKL");
}

TEST(screen, Screen__scrollback_empty) {
    SCREEN(s, 10, 3, (size_t)50);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    s.scroll(Screen::Scroll::deltaRow(1));
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");
}

TEST(screen, Screen__scrollback_doesn_t_move_viewport_if_not_at_bottom) {
    SCREEN(s, 10, 3, (size_t)3);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH");

    /* First test: we scroll up by 1, so we're not at the bottom anymore. */
    s.scroll(Screen::Scroll::deltaRow(-1));
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n4ABCD");

    /* Next, we scroll back down by 1, this grows the scrollback but we
     * shouldn't move. */
    ASSERT_TRUE(s.cursorDownScroll());
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n4ABCD");

    /* Scroll again, this clears scrollback so we should move viewports
     * but still see the same thing since our original view fits. */
    ASSERT_TRUE(s.cursorDownScroll());
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n4ABCD");
}

TEST(screen, Screen__scrolling_moves_selection) {
    SCREEN(s, 5, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Select a single line */
    ASSERT_TRUE(s.select(
        Selection::init(pinAt(s, Point::active(0, 1)), pinAt(s, Point::active(s.pages.cols - 1, 1)), false)));

    /* Scroll down, should still be bottom */
    ASSERT_TRUE(s.cursorDownScroll());

    /* Our selection should've moved up */
    {
        const Selection sel = s.selection.value;
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s.pages.cols - 1, 0)));
    }

    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");

    /* Scrolling to the bottom does nothing */
    s.scroll(Screen::Scroll::active());

    /* Our selection should've stayed the same */
    {
        const Selection sel = s.selection.value;
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s.pages.cols - 1, 0)));
    }

    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");

    /* Scroll up again */
    ASSERT_TRUE(s.cursorDownScroll());

    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "3IJKL");

    /* Our selection should be null because it left the screen. */
    {
        const Selection sel = s.selection.value;
        ASSERT_TRUE(!s.pages.pointFromPin(point::Tag::active, sel.start()).has);
        ASSERT_TRUE(!s.pages.pointFromPin(point::Tag::active, sel.end()).has);
    }
}

TEST(screen, Screen__cursorScrollRegionUp_simple) {
    SCREEN(s, 5, 5, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4MNOP\n5QRST");

    /* Scroll a region ending at row 2 (zero-indexed) up by one. This
     * emulates a scroll region of rows 0-2 with the cursor at the
     * region bottom. */
    s.cursorAbsolute(1, 2);
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    /* The cursor stays in place, on the new blank row. */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);

    /* Rows in the region scrolled, rows below are unchanged, and
     * nothing was moved into scrollback. */
    EXPECT_DUMP(s, screen, "2EFGH\n3IJKL\n\n4MNOP\n5QRST");
}

TEST(screen, Screen__cursorScrollRegionUp_renews_page_generation) {
    SCREEN(s, 5, 5, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4MNOP\n5QRST");
    s.cursorAbsolute(0, 2);

    PageList::Node *node = s.cursor.page_pin->node;
    const uint64_t serial = node->serial;
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    ASSERT_TRUE(!s.pages.nodeIsValid(node, serial));
}

TEST(screen, Screen__cursorScrollRegionUp_moves_selection) {
    SCREEN(s, 5, 5, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4MNOP\n5QRST");

    /* Select the second row. */
    ASSERT_TRUE(s.select(
        Selection::init(pinAt(s, Point::active(0, 1)), pinAt(s, Point::active(s.pages.cols - 1, 1)), false)));

    s.cursorAbsolute(0, 2);
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    /* Our selection should've moved up with its row. */
    {
        const Selection sel = s.selection.value;
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s.pages.cols - 1, 0)));
    }

    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n\n4MNOP\n5QRST");
}

/* Wisp: shared setup of the cursorScrollRegionUp "spans pages" tests.
 * We need to get the cursor to a new page. */
static bool fillThreeToFirstPage(Screen &s) {
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    s.pages.pages.first->page()->pauseIntegrityChecks(true);
    for (size_t i = 0; i < first_page_size - 3; i++)
        if (!s.testWriteString("\n")) return false;
    s.pages.pages.first->page()->pauseIntegrityChecks(false);
    return s.testWriteString("1A\n2B\n3C\n4D\n5E");
}

TEST(screen, Screen__cursorScrollRegionUp_region_spans_pages) {
    SCREEN(s, 10, 5, (size_t)10);

    /* We need to get the cursor to a new page */
    ASSERT_TRUE(fillThreeToFirstPage(s));

    /* Move the cursor to the first row of the second page and give it
     * a non-default style. This is important: it verifies that the
     * cursor's style ref stays accounted on the correct page even
     * though eraseRowBounded moves the cursor's tracked pin across
     * the page boundary. */
    s.cursorAbsolute(0, 3);
    NOERR(s.setAttribute(attr(A::bold)));
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.last);
    ASSERT_TRUE(0 == s.cursor.page_pin->y);

    /* Scroll a region of active rows 1-3 with the cursor at the region
     * bottom. The region spans the page boundary so this exercises the
     * slow path. */
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    /* The cursor stays in place, on the new blank row. */
    ASSERT_TRUE(0 == s.cursor.x);
    ASSERT_TRUE(3 == s.cursor.y);

    EXPECT_DUMP(s, viewport, "1A\n3C\n4D\n\n5E");

    /* Our cursor style must remain usable: write a styled cell and
     * verify the style ref counting is intact on the cursor's page. */
    WRITE(s, "X");
    {
        Page *page = s.cursor.page_pin->node->page();
        const size_t styles = page->styles.count();
        ASSERT_TRUE(1 == styles);
    }
}

TEST(screen, Screen__cursorScrollRegionUp_region_spans_pages_with_background_SGR) {
    SCREEN(s, 10, 5, (size_t)10);

    /* We need to get the cursor to a new page. See the previous test
     * for a diagram of the page layout. */
    ASSERT_TRUE(fillThreeToFirstPage(s));

    s.cursorAbsolute(0, 3);
    NOERR(s.setAttribute(terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 0xFF, 0, 0)));
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.last);
    ASSERT_TRUE(0 == s.cursor.page_pin->y);

    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    EXPECT_DUMP(s, viewport, "1A\n3C\n4D\n\n5E");

    /* The new blank row must be filled with our background color. */
    for (size_t x = 0; x < s.pages.cols; x++) {
        const PageList::Cell list_cell = s.pages.getCell(Point::active((uint32_t)x, 3)).value;
        ASSERT_TRUE(list_cell.cell->content_tag() == Cell::ContentTag::bg_color_rgb);
        ASSERT_TRUE(rgbEq(list_cell.cell->contentColorRgb(), 0xFF, 0, 0));
    }
}

TEST(screen, Screen__cursorScrollRegionUp_with_styled_erased_row) {
    SCREEN(s, 5, 3, (size_t)0);

    /* Write a styled row at the top so the erased row has managed
     * memory that must be released. */
    NOERR(s.setAttribute(attr(A::bold)));
    WRITE(s, "1ABCD");
    NOERR(s.setAttribute(attr(A::unset)));
    WRITE(s, "\n2EFGH\n3IJKL");

    s.cursorAbsolute(0, 2);
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    EXPECT_DUMP(s, screen, "2EFGH\n3IJKL");

    /* The style should be gone from the page since the only user
     * was the erased row. */
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(0 == page->styles.count());
}

TEST(screen, Screen__scrolling_moves_viewport) {
    SCREEN(s, 10, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n");
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    s.scroll(Screen::Scroll::deltaRow(-2));

    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n1ABCD");

    ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, s.pages.getTopLeft(point::Tag::viewport)),
                     Point::screen(0, 1)));
}

TEST(screen, Screen__scrolling_when_viewport_is_pruned) {
    SCREEN(s, 215, 3, (size_t)1);

    /* Write some to create scrollback and move back into our scrollback. */
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n");
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    s.scroll(Screen::Scroll::deltaRow(-2));

    /* Our viewport is now somewhere pinned. Create so much scrollback
     * that we prune it. */
    WRITE(s, "\n");
    for (int i = 0; i < 1000; i++) WRITE(s, "1ABCD\n2EFGH\n3IJKL\n");
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, s.pages.getTopLeft(point::Tag::viewport)),
                     Point::screen(0, 0)));
}

TEST(screen, Screen__scroll_and_clear_full_screen) {
    SCREEN(s, 10, 3, (size_t)5);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    ASSERT_TRUE(s.scrollClear());
    EXPECT_DUMP(s, viewport, "");
    EXPECT_DUMP(s, screen, "1ABCD\n2EFGH\n3IJKL");
}

TEST(screen, Screen__scroll_and_clear_partial_screen) {
    SCREEN(s, 10, 3, (size_t)5);
    WRITE(s, "1ABCD\n2EFGH");

    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH");

    ASSERT_TRUE(s.scrollClear());
    EXPECT_DUMP(s, viewport, "");
    EXPECT_DUMP(s, screen, "1ABCD\n2EFGH");
}

TEST(screen, Screen__scroll_and_clear_empty_screen) {
    SCREEN(s, 10, 3, (size_t)5);
    ASSERT_TRUE(s.scrollClear());
    EXPECT_DUMP(s, viewport, "");
    EXPECT_DUMP(s, screen, "");
}

TEST(screen, Screen__scroll_and_clear_ignore_blank_lines) {
    SCREEN(s, 10, 3, (size_t)10);
    WRITE(s, "1ABCD\n2EFGH");
    ASSERT_TRUE(s.scrollClear());
    EXPECT_DUMP(s, viewport, "");

    /* Move back to top-left */
    s.cursorAbsolute(0, 0);

    /* Write and clear */
    WRITE(s, "3ABCD\n");
    EXPECT_DUMP(s, active, "3ABCD");

    ASSERT_TRUE(s.scrollClear());
    EXPECT_DUMP(s, viewport, "");

    /* Move back to top-left */
    s.cursorAbsolute(0, 0);
    WRITE(s, "X");

    EXPECT_DUMP(s, screen, "1ABCD\n2EFGH\n3ABCD\nX");
}

/* Wisp: shared setup. Write n blank lines with the first page's
 * integrity checks paused. */
static bool writeBlankLines(Screen &s, size_t n) {
    s.pages.pages.first->page()->pauseIntegrityChecks(true);
    for (size_t i = 0; i < n; i++)
        if (!s.testWriteString("\n")) return false;
    s.pages.pages.first->page()->pauseIntegrityChecks(false);
    return true;
}

#define EXPECT_BG_155(s, x, y)                                                                                         \
    do {                                                                                                               \
        const PageList::Cell _lc = (s).pages.getCell(Point::active((x), (y))).value;                                  \
        ASSERT_TRUE(_lc.cell->content_tag() == Cell::ContentTag::bg_color_rgb);                                      \
        ASSERT_TRUE(rgbEq(_lc.cell->contentColorRgb(), 155, 0, 0));                                                   \
    } while (0)

#define BG_155 terminal::sgr::Attribute::makeRgb(A::direct_color_bg, 155, 0, 0)

TEST(screen, Screen__scroll_above_same_page) {
    SCREEN(s, 10, 3, (size_t)10);
    NOERR(s.setAttribute(BG_155));
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    s.cursorAbsolute(0, 1);
    s.pages.clearDirty();

    PageList::Node *node = s.cursor.page_pin->node;
    const uint64_t serial = node->serial;
    ASSERT_TRUE(s.cursorScrollAbove());
    ASSERT_TRUE(!s.pages.nodeIsValid(node, serial));

    EXPECT_DUMP(s, viewport, "2EFGH\n\n3IJKL");
    EXPECT_BG_155(s, 0, 1);

    /* Page 0 row 1 (active row 0) is dirty because the cursor moved off of it. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    /* Page 0 row 2 (active row 1) is dirty because it was cleared. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    /* Page 0 row 3 (active row 2) is dirty because it's new. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));
}

TEST(screen, Screen__scroll_above_same_page_but_cursor_on_previous_page) {
    SCREEN(s, 10, 5, (size_t)10);

    /* We need to get the cursor to a new page */
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_size - 3));

    NOERR(s.setAttribute(BG_155));
    WRITE(s, "1A\n2B\n3C\n4D\n5E");
    s.cursorAbsolute(0, 1);
    s.pages.clearDirty();

    /* Ensure we're still on the first page and have a second */
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);
    ASSERT_TRUE(s.pages.pages.first->next != nullptr);

    PageList::Node *first_node = s.pages.pages.first;
    PageList::Node *second_node = first_node->next;
    const uint64_t first_serial = first_node->serial;
    const uint64_t second_serial = second_node->serial;
    ASSERT_TRUE(s.cursorScrollAbove());
    ASSERT_TRUE(!s.pages.nodeIsValid(first_node, first_serial));
    ASSERT_TRUE(!s.pages.nodeIsValid(second_node, second_serial));

    EXPECT_DUMP(s, viewport, "2B\n\n3C\n4D\n5E");
    EXPECT_BG_155(s, 0, 1);

    /* Page 0's penultimate row is dirty because the cursor moved off of it. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    /* The rest of the rows are dirty because they've been modified or are new. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 3)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 4)));
}

TEST(screen, Screen__scroll_above_same_page_but_cursor_on_previous_page_last_row) {
    SCREEN(s, 10, 5, (size_t)10);

    /* We need to get the cursor to a new page */
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_size - 2));

    NOERR(s.setAttribute(BG_155));
    WRITE(s, "1A\n2B\n3C\n4D\n5E");
    s.cursorAbsolute(0, 1);
    s.pages.clearDirty();

    /* Ensure we're still on the first page and have a second */
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);
    ASSERT_TRUE(s.pages.pages.first->next != nullptr);

    ASSERT_TRUE(s.cursorScrollAbove());

    EXPECT_DUMP(s, viewport, "2B\n\n3C\n4D\n5E");
    EXPECT_BG_155(s, 0, 1);

    /* Page 0's final row is dirty because the cursor moved off of it. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    /* Page 1's rows are all dirty because every row was moved. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 3)));
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 4)));

    /* Attempt to clear the style from the cursor and
     * then assert the integrity of both of our pages.
     *
     * This catches a case of memory corruption where the cursor
     * is moved between pages without accounting for style refs. */
    NOERR(s.setAttribute(attr(A::reset_bg)));
    s.pages.pages.first->page()->assertIntegrity();
    s.pages.pages.last->page()->assertIntegrity();
}

TEST(screen, Screen__scroll_above_creates_new_page) {
    SCREEN(s, 10, 3, (size_t)10);

    /* We need to get the cursor to a new page */
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_size - 3));

    NOERR(s.setAttribute(BG_155));
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    s.cursorAbsolute(0, 1);
    s.pages.clearDirty();

    /* Ensure we're still on the first page */
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);

    PageList::Node *node = s.pages.pages.first;
    const uint64_t serial = node->serial;
    ASSERT_TRUE(s.cursorScrollAbove());
    ASSERT_TRUE(!s.pages.nodeIsValid(node, serial));

    EXPECT_DUMP(s, viewport, "2EFGH\n\n3IJKL");
    EXPECT_BG_155(s, 0, 1);

    /* Page 0's penultimate row is dirty because the cursor moved off of it. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    /* Page 0's final row is dirty because it was cleared. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    /* Page 1's row is dirty because it's new. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));
}

TEST(screen, Screen__scroll_above_with_cursor_on_non_final_row) {
    SCREEN(s, 10, 4, (size_t)10);

    /* Get the cursor to be 2 rows above a new page */
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_size - 3));

    /* Write 3 lines of text, forcing the last line into the first
     * row of a new page. Move our cursor onto the previous page. */
    NOERR(s.setAttribute(BG_155));
    WRITE(s, "1AB\n2BC\n3DE\n4FG");
    s.cursorAbsolute(0, 1);
    s.pages.clearDirty();

    /* Ensure we're still on the first page. So our cursor is on the first
     * page but we have two pages of data. */
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);

    ASSERT_TRUE(s.cursorScrollAbove());

    EXPECT_DUMP(s, viewport, "2BC\n\n3DE\n4FG");
    EXPECT_BG_155(s, 0, 1);

    /* Page 0's penultimate row is dirty because the cursor moved off of it. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    /* Page 0's final row is dirty because it was cleared. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    /* Page 1's row is dirty because it's new. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));
}

TEST(screen, Screen__scroll_above_no_scrollback_bottom_of_page) {
    SCREEN(s, 10, 3, (size_t)0);

    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_size - 3));

    NOERR(s.setAttribute(BG_155));
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");
    s.cursorAbsolute(0, 1);
    s.pages.clearDirty();

    ASSERT_TRUE(s.cursorScrollAbove());

    EXPECT_DUMP(s, viewport, "2EFGH\n\n3IJKL");
    EXPECT_BG_155(s, 0, 1);

    /* Page 0 row 1 (active row 0) is dirty because the cursor moved off of it. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 0)));
    /* Page 0 row 2 (active row 1) is dirty because it was cleared. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 1)));
    /* Page 0 row 3 (active row 2) is dirty because it is new. */
    ASSERT_TRUE(s.pages.isDirty(Point::active(0, 2)));
}

/* Wisp: every cell flagged as a hyperlink must resolve to a real entry
 * in its page's hyperlink map. */
static bool allHyperlinksResolve(Screen &s) {
    for (PageList::Node *node = s.pages.pages.first; node; node = node->next) {
        Page *page = node->page();
        page->assertIntegrity();
        for (size_t y = 0; y < page->size.rows; y++) {
            const Row *row = page->getRow(y);
            if (!row->hyperlink()) continue;
            const Cell *cells = page->getCells(row);
            for (size_t x = 0; x < page->size.cols; x++) {
                if (!cells[x].hyperlink()) continue;
                hyperlink::Id id;
                if (!page->lookupHyperlink(&cells[x], &id)) return false;
            }
        }
    }
    return true;
}

TEST(screen, Screen__scroll_above_hyperlink_dense_row_to_fresh_page) {
    /* Regression test for https://github.com/ghostty-org/ghostty/discussions/13160
     *
     * When a scroll-above operation pushes a row carrying more unique
     * hyperlinks than a fresh page's default hyperlink capacity across
     * a page boundary, the cross-page row clone must increase the
     * destination page's capacity (like insertLines/deleteLines do)
     * rather than error out mid-operation, which leaves the page list
     * half-mutated and aborts later (e.g. in clearCells). */
    SCREEN(s, 10, 5, (size_t)10000);

    /* Fill the first page so it is exactly full and the cursor is on
     * its last row (which is also the bottom row of the active area).
     * The next grow() will then allocate a fresh page. */
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_rows - 1));
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    ASSERT_TRUE(s.pages.pages.first->capacity().rows == s.pages.pages.first->page()->size.rows);
    ASSERT_TRUE(s.pages.rows - 1 == s.cursor.y);

    /* Fill the bottom row with unique hyperlinks: more than a fresh
     * page can hold with default hyperlink capacity. */
    for (size_t i = 0; i < s.pages.cols; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", i);
        NOERR(startLink(s, buf));
        WRITE(s, "A");
        s.endHyperlink();
    }
    ASSERT_TRUE((size_t)s.pages.cols == s.cursor.page_pin->node->page()->hyperlink_set.count());

    /* Move the cursor above the bottom row and scroll. The dense row is
     * pushed across the page boundary into the freshly allocated page. */
    s.cursorAbsolute(0, 1);
    ASSERT_TRUE(s.cursorScrollAbove());

    /* We must have created a second page and the dense row must now be
     * the top row of that page. */
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);

    /* All hyperlinks must have survived the scroll intact: every cell
     * flagged as a hyperlink must resolve to a real entry in its page's
     * hyperlink map. A half-applied scroll leaves cells whose hyperlink
     * flag is set but that have no map entry, which aborts in
     * clearCells later. */
    ASSERT_TRUE(allHyperlinksResolve(s));
    {
        Page *last_page = s.pages.pages.last->page();
        ASSERT_TRUE((size_t)s.pages.cols == last_page->hyperlink_set.count());
    }

    /* The dense row is still the bottom row of the active area. */
    for (size_t x = 0; x < s.pages.cols; x++) {
        const PageList::Cell list_cell = s.pages.getCell(Point::active((uint32_t)x, 4)).value;
        ASSERT_TRUE(list_cell.cell->hyperlink());
        Page *page = list_cell.node->page();
        hyperlink::Id id;
        ASSERT_TRUE(page->lookupHyperlink(list_cell.cell, &id));
        const hyperlink::PageEntry *link = page->hyperlink_set.get((const void *)page->memory, id);
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", x);
        EXPECT_STR(buf, std::string((const char *)link->uri.slice((const void *)page->memory), link->uri.len));
    }
}

TEST(screen, Screen__scroll_above_hyperlink_dense_row_to_existing_page) {
    /* Same as the fresh page variant above but the destination page
     * already exists (fresh_node == null path in cursorScrollAboveRotate). */
    SCREEN(s, 10, 5, (size_t)10000);

    /* Fill the first page so it is exactly full and the cursor is on
     * its last row. */
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;
    ASSERT_TRUE(writeBlankLines(s, first_page_rows - 1));
    ASSERT_TRUE(s.pages.rows - 1 == s.cursor.y);

    /* Fill the last row of the first page with unique hyperlinks:
     * more than a page can hold with default hyperlink capacity. */
    for (size_t i = 0; i < s.pages.cols; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", i);
        NOERR(startLink(s, buf));
        WRITE(s, "A");
        s.endHyperlink();
    }

    /* Scroll twice so the active area straddles the page boundary:
     * the last two active rows are on a second page while the dense
     * row remains the last row of the first page. */
    WRITE(s, "\n\n");
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.last);

    /* Move the cursor to an active row that is still on the first page
     * and above the dense row, then scroll. grow() has capacity in the
     * last page so no fresh page is allocated, but the dense row still
     * crosses the page boundary during the rotate. */
    s.cursorAbsolute(0, 0);
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);
    ASSERT_TRUE(s.cursorScrollAbove());

    /* All hyperlinks must have survived the scroll intact: every cell
     * flagged as a hyperlink must resolve to a real entry in its page's
     * hyperlink map. */
    ASSERT_TRUE(allHyperlinksResolve(s));
    {
        Page *last_page = s.pages.pages.last->page();
        ASSERT_TRUE((size_t)s.pages.cols == last_page->hyperlink_set.count());
    }
}

/* Wisp: var s2 = try s.clone(io, alloc, top, bot); defer s2.deinit(); */
struct CloneHolder {
    Screen s;
    bool ok;
    CloneHolder(const Screen &src, const Point &top, Maybe<Point> bot) {
        ok = src.clone(talloc(), top, bot, &s) == page::PageError::none;
    }
    ~CloneHolder() {
        if (ok) s.deinit();
    }
};
#define CLONE(var, src, top, bot)                                                                                      \
    CloneHolder var##_holder((src), (top), (bot));                                                                     \
    ASSERT_TRUE(var##_holder.ok);                                                                                      \
    Screen &var = var##_holder.s

static Screen::Resize rsz(size::CellCountInt cols, size::CellCountInt rows, bool reflow = true,
                          bool pull_scrollback = true) {
    Screen::Resize r(cols, rows, reflow);
    r.pull_scrollback = pull_scrollback;
    return r;
}

TEST(screen, Screen__clone) {
    SCREEN(s, 10, 3, (size_t)10);
    WRITE(s, "1ABCD\n2EFGH");
    EXPECT_DUMP(s, active, "1ABCD\n2EFGH");
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    /* Clone */
    CLONE(s2, s, Point::active(), Maybe<Point>());
    EXPECT_DUMP(s2, active, "1ABCD\n2EFGH");
    ASSERT_TRUE(5 == s2.cursor.x);
    ASSERT_TRUE(1 == s2.cursor.y);

    /* Write to s1, should not be in s2 */
    WRITE(s, "\n34567");
    EXPECT_DUMP(s, active, "1ABCD\n2EFGH\n34567");
    EXPECT_DUMP(s2, active, "1ABCD\n2EFGH");
    ASSERT_TRUE(5 == s2.cursor.x);
    ASSERT_TRUE(1 == s2.cursor.y);
}

TEST(screen, Screen__clone_partial) {
    SCREEN(s, 10, 3, (size_t)10);
    WRITE(s, "1ABCD\n2EFGH");
    EXPECT_DUMP(s, active, "1ABCD\n2EFGH");
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    /* Clone */
    CLONE(s2, s, Point::active(0, 1), Maybe<Point>());
    EXPECT_DUMP(s2, active, "2EFGH");

    /* Cursor is shifted since we cloned partial */
    ASSERT_TRUE(5 == s2.cursor.x);
    ASSERT_TRUE(0 == s2.cursor.y);
}

TEST(screen, Screen__clone_partial_cursor_out_of_bounds) {
    SCREEN(s, 10, 3, (size_t)10);
    WRITE(s, "1ABCD\n2EFGH");
    EXPECT_DUMP(s, active, "1ABCD\n2EFGH");
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    /* Clone */
    CLONE(s2, s, Point::active(0, 0), Point::active(0, 0));
    EXPECT_DUMP(s2, active, "1ABCD");

    /* Cursor is shifted since we cloned partial */
    ASSERT_TRUE(0 == s2.cursor.x);
    ASSERT_TRUE(0 == s2.cursor.y);
}

TEST(screen, Screen__clone_contains_full_selection) {
    SCREEN(s, 5, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Select a single line */
    ASSERT_TRUE(s.select(
        Selection::init(pinAt(s, Point::active(0, 1)), pinAt(s, Point::active(s.pages.cols - 1, 1)), false)));

    /* Clone */
    CLONE(s2, s, Point::active(), Maybe<Point>());

    /* Our selection should remain valid */
    {
        const Selection sel = s2.selection.value;
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 1)));
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s2.pages.cols - 1, 1)));
    }
}

TEST(screen, Screen__clone_contains_none_of_selection) {
    SCREEN(s, 5, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Select a single line */
    ASSERT_TRUE(s.select(
        Selection::init(pinAt(s, Point::active(0, 0)), pinAt(s, Point::active(s.pages.cols - 1, 0)), false)));

    /* Clone */
    CLONE(s2, s, Point::active(0, 1), Maybe<Point>());

    /* Our selection should be null */
    ASSERT_TRUE(!s2.selection.has);
}

TEST(screen, Screen__clone_contains_selection_start_cutoff) {
    SCREEN(s, 5, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Select a single line */
    ASSERT_TRUE(s.select(
        Selection::init(pinAt(s, Point::active(0, 0)), pinAt(s, Point::active(s.pages.cols - 1, 1)), false)));

    /* Clone */
    CLONE(s2, s, Point::active(0, 1), Maybe<Point>());

    /* Our selection should remain valid */
    {
        const Selection sel = s2.selection.value;
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s2.pages.cols - 1, 0)));
    }
}

TEST(screen, Screen__clone_contains_selection_end_cutoff) {
    SCREEN(s, 5, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Select a single line */
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(0, 1)), pinAt(s, Point::active(2, 2)), false)));

    /* Clone */
    CLONE(s2, s, Point::active(0, 0), Point::active(0, 1));

    /* Our selection should remain valid */
    {
        const Selection sel = s2.selection.value;
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 1)));
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s2.pages.cols - 1, 2)));
    }
}

TEST(screen, Screen__clone_contains_selection_end_cutoff_reversed) {
    SCREEN(s, 5, 3, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    /* Select a single line */
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(2, 2)), pinAt(s, Point::active(0, 1)), false)));

    /* Clone */
    CLONE(s2, s, Point::active(0, 0), Point::active(0, 1));

    /* Our selection should remain valid */
    {
        const Selection sel = s2.selection.value;
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 1)));
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s2.pages.cols - 1, 2)));
    }
}

TEST(screen, Screen__clone_contains_subset_of_selection) {
    SCREEN(s, 5, 4, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4ABCD");

    /* Select the full screen */
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(0, 0)), pinAt(s, Point::active(0, 3)), false)));

    /* Clone */
    CLONE(s2, s, Point::active(0, 1), Point::active(0, 2));

    /* Our selection should remain valid */
    {
        const Selection sel = s2.selection.value;
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(s2.pages.cols - 1, 3)));
    }
}

TEST(screen, Screen__clone_clamps_clipped_selections_to_mixed_width_pages) {
    SCREEN(s, 4, 3, (size_t)0);

    PageList::Node *first = s.pages.pages.first;
    ASSERT_TRUE(s.pages.split(Pin(first, 2)) == PageList::SplitError::none);
    ASSERT_TRUE(s.pages.split(Pin(first, 1)) == PageList::SplitError::none);
    PageList::Node *middle = first->next;
    PageList::Node *last = middle->next;
    middle->page()->size.cols = 2;

    ASSERT_TRUE(s.select(Selection::init(Pin(first), Pin(last, 0, 3), false)));
    {
        CLONE(linear, s, Point::screen(), Point::screen(0, 1));
        const Pin linear_end = linear.selection.value.end();
        (void)linear_end.rowAndCell();
        ASSERT_TRUE(1 == linear_end.x);
    }

    ASSERT_TRUE(s.select(Selection::init(Pin(first, 0, 3), Pin(last, 0, 3), true)));
    {
        CLONE(rectangle, s, Point::screen(0, 1), Maybe<Point>());
        const Pin rectangle_start = rectangle.selection.value.start();
        (void)rectangle_start.rowAndCell();
        ASSERT_TRUE(1 == rectangle_start.x);
    }
}

TEST(screen, Screen__clone_contains_subset_of_rectangle_selection) {
    SCREEN(s, 5, 4, (size_t)1);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4ABCD");

    /* Select the full screen from x=1 to x=3 */
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(1, 0)), pinAt(s, Point::active(3, 3)), true)));

    /* Clone */
    CLONE(s2, s, Point::active(0, 1), Point::active(0, 2));

    /* Our selection should remain valid and be properly clipped
     * preserving the columns of the start and end points of the
     * selection. */
    {
        const Selection sel = s2.selection.value;
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(1, 0)));
        ASSERT_TRUE(ptEq(s2.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(3, 3)));
    }
}

TEST(screen, Screen__clone_basic) {
    SCREEN(s, 10, 3, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    {
        CLONE(s2, s, Point::active(0, 1), Point::active(0, 1));

        /* Test our contents rotated */
        EXPECT_DUMP(s2, active, "2EFGH");
    }

    {
        CLONE(s2, s, Point::active(0, 1), Point::active(0, 2));

        /* Test our contents rotated */
        EXPECT_DUMP(s2, active, "2EFGH\n3IJKL");
    }
}

TEST(screen, Screen__clone_empty_viewport) {
    SCREEN(s, 10, 3, (size_t)0);

    {
        CLONE(s2, s, Point::viewport(0, 0), Point::viewport(0, 0));

        /* Test our contents rotated */
        EXPECT_DUMP(s2, viewport, "");
    }
}

TEST(screen, Screen__clone_one_line_viewport) {
    SCREEN(s, 10, 3, (size_t)0);
    WRITE(s, "1ABC");

    {
        CLONE(s2, s, Point::viewport(0, 0), Point::viewport(0, 0));

        /* Test our contents */
        EXPECT_DUMP(s2, viewport, "1ABC");
    }
}

TEST(screen, Screen__clone_empty_active) {
    SCREEN(s, 10, 3, (size_t)0);

    {
        CLONE(s2, s, Point::active(0, 0), Point::active(0, 0));

        /* Test our contents rotated */
        EXPECT_DUMP(s2, active, "");
    }
}

TEST(screen, Screen__clone_one_line_active_with_extra_space) {
    SCREEN(s, 10, 3, (size_t)0);
    WRITE(s, "1ABC");

    {
        CLONE(s2, s, Point::active(0, 0), Maybe<Point>());

        /* Test our contents rotated */
        EXPECT_DUMP(s2, active, "1ABC");
    }
}

TEST(screen, Screen__clear_history_with_no_history) {
    SCREEN(s, 10, 3, (size_t)3);
    WRITE(s, "4ABCD\n5EFGH\n6IJKL");
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);
    s.eraseHistory(Maybe<Point>());
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);
    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "4ABCD\n5EFGH\n6IJKL");
    /* Test our contents rotated */
    EXPECT_DUMP(s, screen, "4ABCD\n5EFGH\n6IJKL");
}

TEST(screen, Screen__clear_history) {
    SCREEN(s, 10, 3, (size_t)3);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH\n6IJKL");
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);

    /* Scroll to top */
    s.scroll(Screen::Scroll::top());
    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    s.eraseHistory(Maybe<Point>());
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);
    /* Test our contents rotated */
    EXPECT_DUMP(s, viewport, "4ABCD\n5EFGH\n6IJKL");
    /* Test our contents rotated */
    EXPECT_DUMP(s, screen, "4ABCD\n5EFGH\n6IJKL");
}

TEST(screen, Screen__clear_above_cursor) {
    SCREEN(s, 10, 10, (size_t)3);
    WRITE(s, "4ABCD\n5EFGH\n6IJKL");
    s.clearRows(Point::active(0, 0), Point::active(0, s.cursor.y - 1), false);
    EXPECT_DUMP(s, viewport, "\n\n6IJKL");
    EXPECT_DUMP(s, screen, "\n\n6IJKL");

    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
}

TEST(screen, Screen__clear_above_cursor_with_history) {
    SCREEN(s, 10, 3, (size_t)3);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n");
    WRITE(s, "4ABCD\n5EFGH\n6IJKL");
    s.clearRows(Point::active(0, 0), Point::active(0, s.cursor.y - 1), false);
    EXPECT_DUMP(s, viewport, "\n\n6IJKL");
    EXPECT_DUMP(s, screen, "1ABCD\n2EFGH\n3IJKL\n\n\n6IJKL");

    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
}

TEST(screen, Screen__resize__no_reflow__more_rows) {
    SCREEN(s, 10, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(10, 10, false)));
    EXPECT_DUMP(s, viewport, str);
}

TEST(screen, Screen__resize__no_reflow__less_rows) {
    SCREEN(s, 10, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
    ASSERT_TRUE(s.resize(rsz(10, 2, false)));

    /* Since we shrunk, we should adjust our cursor */
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL");
}

/* Wisp: write only a background color into the remaining rows. */
static void fillBgRows(Screen &s) {
    for (size_t y = 1; y < s.pages.rows; y++) {
        const PageList::Cell list_cell = s.pages.getCell(Point::active(0, (uint32_t)y)).value;
        Cell c;
        c.setContentTag(Cell::ContentTag::bg_color_rgb);
        Cell::RGB rgb;
        rgb.r = 0xFF;
        rgb.g = 0;
        rgb.b = 0;
        c.setContentColorRgb(rgb);
        *list_cell.cell = c;
    }
}

TEST(screen, Screen__resize__no_reflow__less_rows_trims_blank_lines) {
    SCREEN(s, 10, 3, (size_t)0);
    const char *str = "1ABCD";
    WRITE(s, str);

    /* Write only a background color into the remaining rows */
    fillBgRows(s);

    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(6, 2, false)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, "1ABCD");
}

TEST(screen, Screen__resize__no_reflow__more_rows_trims_blank_lines) {
    SCREEN(s, 10, 3, (size_t)0);
    const char *str = "1ABCD";
    WRITE(s, str);

    /* Write only a background color into the remaining rows */
    fillBgRows(s);

    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(10, 7, false)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, "1ABCD");
}

TEST(screen, Screen__resize__no_reflow__more_cols) {
    SCREEN(s, 10, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(20, 3, false)));
    EXPECT_DUMP(s, viewport, str);
}

TEST(screen, Screen__resize__no_reflow__less_cols) {
    SCREEN(s, 10, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(4, 3, false)));
    EXPECT_DUMP(s, viewport, "1ABC\n2EFG\n3IJK");
}

TEST(screen, Screen__resize__no_reflow__more_rows_with_scrollback_cursor_end) {
    SCREEN(s, 7, 3, (size_t)2);
    const char *str = "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(7, 10, false)));
    EXPECT_DUMP(s, viewport, str);
}

TEST(screen, Screen__resize__no_reflow__more_rows_no_scrollback_pull) {
    SCREEN(s, 7, 3, (size_t)2);
    const char *str = "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);

    /* Cursor is at the bottom so this would normally pull scrollback. */
    ASSERT_TRUE(2 == s.cursor.y);
    ASSERT_TRUE(s.resize(rsz(7, 10, false, false)));
    ASSERT_TRUE(2 == s.cursor.y);
    EXPECT_DUMP(s, active, "3IJKL\n4ABCD\n5EFGH");
    EXPECT_DUMP(s, screen, str);
}

TEST(screen, Screen__resize_more_cols_no_scrollback_pull) {
    SCREEN(s, 5, 3, (size_t)2);
    WRITE(s, "1AAAA\n2BBBB\n3CCCCDD\n4E");
    EXPECT_DUMP(s, active, "3CCCC\nDD\n4E");

    /* The wrapped line in the active area unwraps, freeing up a row. This
     * would normally pull "2BBBB" back but we should get a blank row at
     * the bottom instead. */
    ASSERT_TRUE(s.resize(rsz(10, 3, true, false)));
    ASSERT_TRUE(2 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);
    EXPECT_DUMP(s, active, "3CCCCDD\n4E");
    EXPECT_DUMP(s, screen, "1AAAA\n2BBBB\n3CCCCDD\n4E");
}

TEST(screen, Screen__resize_more_cols_no_scrollback_pull_wrap_straddles_scrollback) {
    SCREEN(s, 5, 3, (size_t)2);
    WRITE(s, "1AAAA\n2BBBBXX\n3C\n4D");
    EXPECT_DUMP(s, active, "XX\n3C\n4D");

    /* The line isn't fully in scrollback so it is allowed to unwrap
     * back into view, but nothing above it is. */
    ASSERT_TRUE(s.resize(rsz(10, 3, true, false)));
    ASSERT_TRUE(2 == s.cursor.y);
    EXPECT_DUMP(s, active, "2BBBBXX\n3C\n4D");
}

TEST(screen, Screen__resize_more_cols_and_rows_no_scrollback_pull) {
    SCREEN(s, 5, 3, (size_t)2);
    WRITE(s, "1AAAA\n2BBBB\n3CCCCDD\n4E");

    ASSERT_TRUE(s.resize(rsz(10, 5, true, false)));
    ASSERT_TRUE(1 == s.cursor.y);
    EXPECT_DUMP(s, active, "3CCCCDD\n4E");
}

TEST(screen, Screen__resize_less_cols_no_scrollback_pull) {
    SCREEN(s, 10, 3, (size_t)2);
    WRITE(s, "0Z\n1AAAA\n2BBBBXX\n3C");

    /* Wrapping needs more rows than we have so the top of the active
     * area still scrolls off as usual. */
    ASSERT_TRUE(s.resize(rsz(5, 3, true, false)));
    ASSERT_TRUE(2 == s.cursor.y);
    EXPECT_DUMP(s, active, "2BBBB\nXX\n3C");
}

TEST(screen, Screen__resize__no_reflow__less_rows_with_scrollback) {
    SCREEN(s, 7, 3, (size_t)2);
    const char *str = "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(7, 2, false)));
    EXPECT_DUMP(s, viewport, "4ABCD\n5EFGH");
}

/* https://github.com/mitchellh/ghostty/issues/1030 */
TEST(screen, Screen__resize__no_reflow__less_rows_with_empty_trailing) {
    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1\n2\n3\n4\n5\n6\n7\n8";
    WRITE(s, str);
    ASSERT_TRUE(s.scrollClear());
    s.cursorAbsolute(0, 0);
    WRITE(s, "A\nB");

    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(5, 2, false)));
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);
    EXPECT_DUMP(s, viewport, "A\nB");
}

TEST(screen, Screen__resize__no_reflow__more_rows_with_soft_wrapping) {
    SCREEN(s, 2, 3, (size_t)3);
    const char *str = "1A2B\n3C4E\n5F6G";
    WRITE(s, str);

    /* Every second row should be wrapped */
    for (uint32_t y = 0; y < 6; y++) {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, y)).value;
        const Row *row = list_cell.row;
        const bool wrapped = (y % 2 == 0);
        ASSERT_TRUE(wrapped == row->wrap());
    }

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(2, 10, false)));
    EXPECT_DUMP(s, viewport, "1A\n2B\n3C\n4E\n5F\n6G");

    /* Every second row should be wrapped */
    for (uint32_t y = 0; y < 6; y++) {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, y)).value;
        const Row *row = list_cell.row;
        const bool wrapped = (y % 2 == 0);
        ASSERT_TRUE(wrapped == row->wrap());
    }
}

TEST(screen, Screen__resize_more_rows_no_scrollback) {
    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(5, 10)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);
}

TEST(screen, Screen__resize_more_rows_with_empty_scrollback) {
    SCREEN(s, 5, 3, (size_t)10);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(5, 10)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);
}

TEST(screen, Screen__resize_more_rows_with_populated_scrollback) {
    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    EXPECT_DUMP(s, viewport, "3IJKL\n4ABCD\n5EFGH");

    /* Set our cursor to be on the "4" */
    s.cursorAbsolute(0, 1);
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::active(s.cursor.x, s.cursor.y)).value;
        ASSERT_TRUE('4' == list_cell.cell->contentCodepoint());
    }

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(5, 10)));

    /* Cursor should still be on the "4" */
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::active(s.cursor.x, s.cursor.y)).value;
        ASSERT_TRUE('4' == list_cell.cell->contentCodepoint());
    }

    EXPECT_DUMP(s, viewport, "3IJKL\n4ABCD\n5EFGH");
}

TEST(screen, Screen__resize_more_cols_no_reflow) {
    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(10, 3)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);
}

/* https://github.com/mitchellh/ghostty/issues/272#issuecomment-1676038963 */
TEST(screen, Screen__resize_more_cols_perfect_split) {
    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD2EFGH3IJKL";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(10, 3)));
    EXPECT_DUMP(s, screen, "1ABCD2EFGH\n3IJKL");
}

/* https://github.com/mitchellh/ghostty/issues/1159 */
TEST(screen, Screen__resize__no_reflow__more_cols_with_scrollback_scrolled_up) {
    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1\n2\n3\n4\n5\n6\n7\n8";
    WRITE(s, str);

    /* Cursor at bottom */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);

    s.scroll(Screen::Scroll::deltaRow(-4));
    EXPECT_DUMP(s, viewport, "2\n3\n4");

    ASSERT_TRUE(s.resize(rsz(8, 3)));
    EXPECT_DUMP(s, screen, str);

    /* Cursor remains at bottom */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
}

/* https://github.com/mitchellh/ghostty/issues/1159 */
TEST(screen, Screen__resize__no_reflow__less_cols_with_scrollback_scrolled_up) {
    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1\n2\n3\n4\n5\n6\n7\n8";
    WRITE(s, str);

    /* Cursor at bottom */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);

    s.scroll(Screen::Scroll::deltaRow(-4));
    EXPECT_DUMP(s, viewport, "2\n3\n4");

    ASSERT_TRUE(s.resize(rsz(4, 3)));
    EXPECT_DUMP(s, screen, str);
    EXPECT_DUMP(s, active, "6\n7\n8");

    /* Cursor remains at bottom */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
}

typedef Screen::SemanticContentSet SCS;
typedef terminal::osc::semantic_prompt::PromptKind PromptKind;

TEST(screen, Screen__resize_more_cols_no_reflow_preserves_semantic_prompt) {
    SCREEN(s, 5, 3, (size_t)0);

    /* Set one of the rows to be a prompt */
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "1ABCD\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "2EFGH");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "\n3IJKL");

    ASSERT_TRUE(s.resize(rsz(10, 3, false)));

    const char *expected = "1ABCD\n2EFGH\n3IJKL";
    EXPECT_DUMP(s, viewport, expected);
    EXPECT_DUMP(s, screen, expected);

    /* Our one row should still be a semantic prompt, the others should not. */
    ASSERT_TRUE(s.pages.getCell(Point::active(0, 0)).value.row->semantic_prompt() == Row::SemanticPrompt::none);
    ASSERT_TRUE(s.pages.getCell(Point::active(0, 1)).value.row->semantic_prompt() == Row::SemanticPrompt::prompt);
    ASSERT_TRUE(s.pages.getCell(Point::active(0, 2)).value.row->semantic_prompt() == Row::SemanticPrompt::none);
}

#define EXPECT_CURSOR_CP(s, ch)                                                                                        \
    ASSERT_TRUE((uint32_t)(ch) ==                                                                                      \
                (s).pages.getCell(Point::active((s).cursor.x, (s).cursor.y)).value.cell->contentCodepoint())

TEST(screen, Screen__resize_more_cols_with_reflow_that_fits_full_width) {
    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD2EFGH\n3IJKL";
    WRITE(s, str);

    /* Verify we soft wrapped */
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Let's put our cursor on row 2, where the soft wrap is */
    s.cursorAbsolute(0, 1);
    EXPECT_CURSOR_CP(s, '2');

    /* Resize and verify we undid the soft wrap because we have space now */
    ASSERT_TRUE(s.resize(rsz(10, 3)));
    EXPECT_DUMP(s, viewport, str);

    /* Our cursor should've moved */
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(0 == s.cursor.y);
}

TEST(screen, Screen__resize_more_cols_with_reflow_that_ends_in_newline) {
    SCREEN(s, 6, 3, (size_t)0);
    const char *str = "1ABCD2EFGH\n3IJKL";
    WRITE(s, str);

    /* Verify we soft wrapped */
    EXPECT_DUMP(s, viewport, "1ABCD2\nEFGH\n3IJKL");

    /* Let's put our cursor on the last row */
    s.cursorAbsolute(0, 2);
    EXPECT_CURSOR_CP(s, '3');

    /* Resize and verify we undid the soft wrap because we have space now */
    ASSERT_TRUE(s.resize(rsz(10, 3)));
    EXPECT_DUMP(s, viewport, str);

    /* Our cursor should still be on the 3 */
    EXPECT_CURSOR_CP(s, '3');
}

TEST(screen, Screen__resize_more_cols_with_reflow_that_forces_more_wrapping) {
    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD2EFGH\n3IJKL";
    WRITE(s, str);

    /* Let's put our cursor on row 2, where the soft wrap is */
    s.cursorAbsolute(0, 1);
    EXPECT_CURSOR_CP(s, '2');

    /* Verify we soft wrapped */
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Resize and verify we undid the soft wrap because we have space now */
    ASSERT_TRUE(s.resize(rsz(7, 3)));
    EXPECT_DUMP(s, viewport, "1ABCD2E\nFGH\n3IJKL");

    /* Our cursor should've moved */
    ASSERT_TRUE(5 == s.cursor.x);
    ASSERT_TRUE(0 == s.cursor.y);
}

TEST(screen, Screen__resize_more_cols_with_reflow_that_unwraps_multiple_times) {
    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD2EFGH3IJKL";
    WRITE(s, str);

    /* Let's put our cursor on row 2, where the soft wrap is */
    s.cursorAbsolute(0, 2);
    EXPECT_CURSOR_CP(s, '3');

    /* Verify we soft wrapped */
    EXPECT_DUMP(s, viewport, "1ABCD\n2EFGH\n3IJKL");

    /* Resize and verify we undid the soft wrap because we have space now */
    ASSERT_TRUE(s.resize(rsz(15, 3)));
    EXPECT_DUMP(s, viewport, "1ABCD2EFGH3IJKL");

    /* Our cursor should've moved */
    ASSERT_TRUE(10 == s.cursor.x);
    ASSERT_TRUE(0 == s.cursor.y);
}

TEST(screen, Screen__resize_more_cols_with_populated_scrollback) {
    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1ABCD\n2EFGH\n3IJKL\n4ABCD5EFGH";
    WRITE(s, str);
    EXPECT_DUMP(s, viewport, "3IJKL\n4ABCD\n5EFGH");

    /* // Set our cursor to be on the "5" */
    s.cursorAbsolute(0, 2);
    EXPECT_CURSOR_CP(s, '5');

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(10, 3)));
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n4ABCD5EFGH");

    /* Cursor should still be on the "5" */
    EXPECT_CURSOR_CP(s, '5');
}

TEST(screen, Screen__resize_more_cols_bounded_scrollback_keeps_viewport_valid) {
    /* Regression test for issue #12298.
     *
     * This needs to live at the Screen layer rather than PageList because the
     * bad state only appears once Screen forwards the active cursor into the
     * resize path. A direct PageList resize repro does not hit the same bug. */
    SCREEN(s, 2, 10, (size_t)10000);

    /* Build 30 rows of scrollback on top of our 10-row viewport so we have a
     * 40-row screen with history above the active area. */
    for (int i = 0; i < 30; i++) {
        PageList::Node *n;
        ASSERT_TRUE(s.pages.grow(&n));
    }
    s.cursorReload();
    ASSERT_TRUE(40 == s.pages.scrollbar().total);

    /* Fill the entire screen with two-row wrapped runs:
     * - even rows mark the end of a wrapped line
     * - odd rows mark the continuation
     *
     * With 2 columns, each logical line occupies two rows. When we grow to 4
     * columns with reflow enabled, those pairs unwrap back into single rows.
     * That cuts the total row count down and is what stresses the viewport pin. */
    PageList::PageIterator it = s.pages.pageIterator(PageList::Direction::right_down, Point::screen(), Maybe<Point>());
    PageList::PageIterator::Chunk chunk;
    while (it.next(&chunk)) {
        Page *page = chunk.node->page();
        for (size_t y = chunk.start; y < chunk.end; y++) {
            const Page::RowAndCell rac = page->getRowAndCell(0, y);
            if (y % 2 == 0) {
                rac.row->setWrap(true);
            } else {
                rac.row->setWrapContinuation(true);
            }
            for (size_t x = 0; x < s.pages.cols; x++) {
                *page->getRowAndCell(x, y).cell = Cell::init('A');
            }
        }
    }

    /* Pin the viewport to a history row just above the active area.
     *
     * Before resize:
     * - total rows = 40
     * - active area starts at row 30
     * - viewport is pinned at row 28
     *
     * After unwrap during resize:
     * - total rows shrinks to 20
     * - the old row 28 remaps into what is now the active area
     *
     * The bug was that resize/grow would temporarily keep the viewport as a
     * history pin even after reflow had moved it into the active area, leaving
     * fewer than `rows` visible rows beneath the pin and tripping integrity
     * checks. */
    s.pages.scroll(PageList::Scroll::pinAt(pinAt(s, Point::screen(0, 28))));
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::pin);
    ASSERT_TRUE(s.pages.getBottomRight(point::Tag::viewport).has);

    /* Growing columns triggers reflow, which unwraps the synthetic wrapped
     * rows above. This used to panic during the resize path. */
    ASSERT_TRUE(s.resize(rsz(4, s.pages.rows, true)));

    /* After the fix, the viewport is normalized back to the active area as
     * soon as the pinned row lands there, so viewport queries remain valid. */
    ASSERT_TRUE(4 == s.pages.cols);
    ASSERT_TRUE(s.pages.scrollbar().total < 40);
    ASSERT_TRUE(s.pages.viewport == PageList::Viewport::active);
    ASSERT_TRUE(s.pages.getBottomRight(point::Tag::viewport).has);
}

TEST(screen, Screen__resize_more_cols_with_reflow) {
    SCREEN(s, 2, 3, (size_t)5);
    const char *str = "1ABC\n2DEF\n3ABC\n4DEF";
    WRITE(s, str);

    /* Let's put our cursor on row 2, where the soft wrap is */
    s.cursorAbsolute(0, 2);
    EXPECT_CURSOR_CP(s, 'E');

    /* Verify we soft wrapped */
    EXPECT_DUMP(s, viewport, "BC\n4D\nEF");

    /* Resize and verify we undid the soft wrap because we have space now */
    ASSERT_TRUE(s.resize(rsz(7, 3)));
    EXPECT_DUMP(s, screen, "1ABC\n2DEF\n3ABC\n4DEF");

    /* Our cursor should've moved */
    ASSERT_TRUE(2 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
}

/* Wisp: std.meta.eql. The compared values are bitwise copies of each
 * other, so comparing their bytes is comparing their fields. */
template <typename T> static bool bytesEq(const T &a, const T &b) { return memcmp(&a, &b, sizeof(T)) == 0; }

TEST(screen, Screen__resize_errors_preserve_state) {
    typedef Screen::resize_tw tw;
    const Screen::ResizeTw tags[] = {Screen::ResizeTw::saved_cursor_pin, Screen::ResizeTw::pages};
    for (size_t ti = 0; ti < 2; ti++) {
        const Screen::ResizeTw tag = tags[ti];
        struct TwEnd {
            ~TwEnd() { (void)tw::end(tripwire::ResetMode::reset); }
        } tw_end;

        SCREEN(s, 10, 3, (size_t)0);

        s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
        WRITE(s, "> ");
        s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
        WRITE(s, "echo");
        NOERR(s.setAttribute(attr(A::bold)));
        NOERR(startLink(s, "https://example.com", "resize"));
        {
            Screen::SavedCursor sc;
            memset(&sc, 0, sizeof sc);
            sc.x = 1;
            sc.y = 0;
            sc.style = s.cursor.style;
            sc.protected_ = s.cursor.protected_;
            sc.pending_wrap = s.cursor.pending_wrap;
            sc.origin = false;
            sc.charset = s.charset;
            s.saved_cursor = sc;
        }

        /* Keep a shallow copy for all non-page state and a byte-for-byte
         * copy of the sole page so reference counts and prompt contents are
         * covered as well. */
        ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
        const Screen before = s;
        const Pin before_viewport_pin = *s.pages.viewport_pin;
        const size_t before_tracked_pins = s.pages.countTrackedPins();
        Page *first_page = s.pages.pages.first->page();
        const std::string before_page((const char *)first_page->memory, first_page->memory_len);

        tw::errorAlways(tag, AllocTw::OutOfMemory);
        Screen::Resize r(20, 4);
        r.prompt_redraw = terminal::osc::semantic_prompt::Redraw::true_;
        ASSERT_TRUE(!s.resize(r));

        ASSERT_TRUE(bytesEq(before.cursor, s.cursor));
        ASSERT_TRUE(bytesEq(before.saved_cursor, s.saved_cursor));
        ASSERT_TRUE(bytesEq(before.selection, s.selection));
        ASSERT_TRUE(bytesEq(before.charset, s.charset));
        ASSERT_TRUE(before.protected_mode == s.protected_mode);
        ASSERT_TRUE(bytesEq(before.kitty_keyboard, s.kitty_keyboard));
        ASSERT_TRUE(bytesEq(before.semantic_prompt, s.semantic_prompt));
        ASSERT_TRUE(bytesEq(before.dirty, s.dirty));
        ASSERT_TRUE(before.pages.pages.first == s.pages.pages.first);
        ASSERT_TRUE(before.pages.pages.last == s.pages.pages.last);
        ASSERT_TRUE(before.pages.cols == s.pages.cols);
        ASSERT_TRUE(before.pages.rows == s.pages.rows);
        ASSERT_TRUE(before.pages.total_rows == s.pages.total_rows);
        ASSERT_TRUE(before.pages.viewport == s.pages.viewport);
        ASSERT_TRUE(bytesEq(before_viewport_pin, *s.pages.viewport_pin));
        ASSERT_TRUE(before_tracked_pins == s.pages.countTrackedPins());
        first_page = s.pages.pages.first->page();
        ASSERT_TRUE(before_page.size() == first_page->memory_len);
        ASSERT_TRUE(memcmp(before_page.data(), first_page->memory, first_page->memory_len) == 0);
    }
}

TEST(screen, Screen__resize_cursor_references_when_node_survives) {

    SCREEN(s, 5, 3, (size_t)1000);

    NOERR(s.setAttribute(attr(A::bold)));
    NOERR(startLink(s, "https://example.com/", "resize"));
    WRITE(s, "abc");

    PageList::Node *original_node = s.cursor.page_pin->node;
    const uint64_t original_serial = original_node->serial;
    {
        Page *page = original_node->page();
        ASSERT_TRUE(4 == page->styles.refCount((const void *)page->memory, s.cursor.style_id));
        ASSERT_TRUE(4 == page->hyperlink_set.refCount((const void *)page->memory, s.cursor.hyperlink_id));
    }

    /* A row-only resize grows the existing page without replacing the
     * cursor's node. The temporary resize references must be released from
     * this page after the cursor state is restored. */
    ASSERT_TRUE(s.resize(rsz(5, 4)));

    ASSERT_TRUE(original_node == s.cursor.page_pin->node);
    ASSERT_TRUE(original_serial == s.cursor.page_pin->node->serial);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(4 == page->styles.refCount((const void *)page->memory, s.cursor.style_id));
        ASSERT_TRUE(4 == page->hyperlink_set.refCount((const void *)page->memory, s.cursor.hyperlink_id));
    }
}

TEST(screen, Screen__resize_cursor_references_when_node_is_replaced) {

    SCREEN(s, 5, 3, (size_t)1000);

    NOERR(s.setAttribute(attr(A::bold)));
    NOERR(startLink(s, "https://example.com/", "resize"));
    WRITE(s, "abc");

    PageList::Node *original_node = s.cursor.page_pin->node;
    const uint64_t original_serial = original_node->serial;
    {
        Page *page = original_node->page();
        ASSERT_TRUE(4 == page->styles.refCount((const void *)page->memory, s.cursor.style_id));
        ASSERT_TRUE(4 == page->hyperlink_set.refCount((const void *)page->memory, s.cursor.hyperlink_id));
    }

    /* A column resize with reflow replaces the page and remaps the tracked
     * cursor pin. The old page owns the temporary references, so destroying
     * it must account for them without attempting to release them afterward. */
    ASSERT_TRUE(s.resize(rsz(10, 3)));

    ASSERT_TRUE(s.cursor.page_pin->node != original_node || s.cursor.page_pin->node->serial != original_serial);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(4 == page->styles.refCount((const void *)page->memory, s.cursor.style_id));
        ASSERT_TRUE(4 == page->hyperlink_set.refCount((const void *)page->memory, s.cursor.hyperlink_id));
    }
}

TEST(screen, Screen__resize_more_rows_and_cols_with_wrapping) {

    SCREEN(s, 2, 4, (size_t)0);
    const char *str = "1A2B\n3C4D";
    WRITE(s, str);
    EXPECT_DUMP(s, viewport, "1A\n2B\n3C\n4D");

    ASSERT_TRUE(s.resize(rsz(5, 10)));

    /* Cursor should move due to wrapping */
    ASSERT_TRUE(3 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);
}

TEST(screen, Screen__resize_less_rows_no_scrollback) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);

    s.cursorAbsolute(0, 0);
    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(5, 1)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, "3IJKL");
    EXPECT_DUMP(s, screen, "3IJKL");
}

TEST(screen, Screen__resize_less_rows_moving_cursor) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);

    /* Put our cursor on the last line */
    s.cursorAbsolute(1, 2);
    EXPECT_CURSOR_CP(s, 'I');

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(5, 1)));

    EXPECT_DUMP(s, viewport, "3IJKL");
    EXPECT_DUMP(s, screen, "3IJKL");

    /* Cursor should be on the last line */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(0 == s.cursor.y);
}

TEST(screen, Screen__resize_less_rows_with_empty_scrollback) {

    SCREEN(s, 5, 3, (size_t)10);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(5, 1)));

    EXPECT_DUMP(s, screen, str);
    EXPECT_DUMP(s, viewport, "3IJKL");
}

TEST(screen, Screen__resize_less_rows_with_populated_scrollback) {

    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    EXPECT_DUMP(s, viewport, "3IJKL\n4ABCD\n5EFGH");

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(5, 1)));

    EXPECT_DUMP(s, screen, str);
    EXPECT_DUMP(s, viewport, "5EFGH");
}

TEST(screen, Screen__resize_less_rows_with_full_scrollback) {

    SCREEN(s, 5, 3, (size_t)3);
    const char *str = "00000\n1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    EXPECT_DUMP(s, viewport, "3IJKL\n4ABCD\n5EFGH");

    ASSERT_TRUE(4 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);

    /* Resize */
    ASSERT_TRUE(s.resize(rsz(5, 2)));

    /* Cursor should stay in the same relative place (bottom of the
     * screen, same character). */
    ASSERT_TRUE(4 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    EXPECT_DUMP(s, screen, "00000\n1ABCD\n2EFGH\n3IJKL\n4ABCD\n5EFGH");
    EXPECT_DUMP(s, viewport, "4ABCD\n5EFGH");
}

TEST(screen, Screen__resize_less_cols_no_reflow) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1AB\n2EF\n3IJ";
    WRITE(s, str);

    s.cursorAbsolute(0, 0);
    const Screen::Cursor cursor = s.cursor;
    ASSERT_TRUE(s.resize(rsz(3, 3)));

    /* Cursor should not move */
    ASSERT_TRUE(cursor.x == s.cursor.x);
    ASSERT_TRUE(cursor.y == s.cursor.y);

    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);
}

TEST(screen, Screen__resize_less_cols_with_reflow_but_row_space) {

    SCREEN(s, 5, 3, (size_t)1);
    const char *str = "1ABCD";
    WRITE(s, str);

    /* Put our cursor on the end */
    s.cursorAbsolute(4, 0);
    EXPECT_CURSOR_CP(s, 'D');

    ASSERT_TRUE(s.resize(rsz(3, 3)));
    EXPECT_DUMP(s, viewport, "1AB\nCD");
    EXPECT_DUMP(s, screen, "1AB\nCD");

    /* Cursor should be on the last line */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);
}

TEST(screen, Screen__resize_less_cols_with_reflow_with_trimmed_rows) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(3, 3)));

    EXPECT_DUMP(s, viewport, "CD\n5EF\nGH");
    EXPECT_DUMP(s, screen, "CD\n5EF\nGH");
}

TEST(screen, Screen__resize_less_cols_with_reflow_with_trimmed_rows_and_scrollback) {

    SCREEN(s, 5, 3, (size_t)1);
    const char *str = "3IJKL\n4ABCD\n5EFGH";
    WRITE(s, str);
    ASSERT_TRUE(s.resize(rsz(3, 3)));

    EXPECT_DUMP(s, viewport, "CD\n5EF\nGH");
    EXPECT_DUMP(s, screen, "3IJ\nKL\n4AB\nCD\n5EF\nGH");
}

TEST(screen, Screen__resize_less_cols_with_reflow_previously_wrapped) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "3IJKL4ABCD5EFGH";
    WRITE(s, str);

    /* Check */
    EXPECT_DUMP(s, screen, "3IJKL\n4ABCD\n5EFGH");

    ASSERT_TRUE(s.resize(rsz(3, 3)));

    /* {
     * const contents = try s.testString(alloc, .viewport);
     * defer alloc.free(contents);
     * const expected = "CD\n5EF\nGH";
     * try testing.expectEqualStrings(expected, contents);
     * } */
    EXPECT_DUMP(s, screen, "ABC\nD5E\nFGH");
}

TEST(screen, Screen__resize_less_cols_with_reflow_and_scrollback) {

    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1A\n2B\n3C\n4D\n5E";
    WRITE(s, str);

    /* Put our cursor on the end */
    s.cursorAbsolute(1, s.pages.rows - 1);
    EXPECT_CURSOR_CP(s, 'E');

    ASSERT_TRUE(s.resize(rsz(3, 3)));

    EXPECT_DUMP(s, viewport, "3C\n4D\n5E");

    /* Cursor should be on the last line */
    ASSERT_TRUE(1 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
}

TEST(screen, Screen__resize_less_cols_with_reflow_previously_wrapped_and_scrollback) {

    SCREEN(s, 5, 3, (size_t)2);
    const char *str = "1ABCD2EFGH3IJKL4ABCD5EFGH";
    WRITE(s, str);

    /* Check */
    EXPECT_DUMP(s, viewport, "3IJKL\n4ABCD\n5EFGH");

    /* Put our cursor on the end */
    s.cursorAbsolute(s.pages.cols - 1, s.pages.rows - 1);
    EXPECT_CURSOR_CP(s, 'H');

    ASSERT_TRUE(s.resize(rsz(3, 3)));

    EXPECT_DUMP(s, viewport, "CD5\nEFG\nH");
    EXPECT_DUMP(s, screen, "1AB\nCD2\nEFG\nH3I\nJKL\n4AB\nCD5\nEFG\nH");

    /* Cursor should be on the last line */
    ASSERT_TRUE(0 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);
    EXPECT_CURSOR_CP(s, 'H');
}

TEST(screen, Screen__resize_less_cols_with_scrollback_keeps_cursor_row) {

    SCREEN(s, 5, 3, (size_t)5);
    const char *str = "1A\n2B\n3C\n4D\n5E";
    WRITE(s, str);

    /* Lets do a scroll and clear operation */
    ASSERT_TRUE(s.scrollClear());

    /* Move our cursor to the beginning */
    s.cursorAbsolute(0, 0);

    ASSERT_TRUE(s.resize(rsz(3, 3)));

    EXPECT_DUMP(s, viewport, "");

    /* Cursor should be on the last line */
    ASSERT_TRUE(0 == s.cursor.x);
    ASSERT_TRUE(0 == s.cursor.y);
}

TEST(screen, Screen__resize_more_rows__less_cols_with_reflow_with_scrollback) {

    SCREEN(s, 5, 3, (size_t)3);
    const char *str = "1ABCD\n2EFGH3IJKL\n4MNOP";
    WRITE(s, str);

    EXPECT_DUMP(s, screen, "1ABCD\n2EFGH\n3IJKL\n4MNOP");
    EXPECT_DUMP(s, viewport, "2EFGH\n3IJKL\n4MNOP");

    ASSERT_TRUE(s.resize(rsz(2, 10)));

    EXPECT_DUMP(s, viewport, "BC\nD\n2E\nFG\nH3\nIJ\nKL\n4M\nNO\nP");
    EXPECT_DUMP(s, screen, "1A\nBC\nD\n2E\nFG\nH3\nIJ\nKL\n4M\nNO\nP");
}

/* This seems like it should work fine but for some reason in practice
 * in the initial implementation I found this bug! This is a regression
 * test for that. */
TEST(screen, Screen__resize_more_rows_then_shrink_again) {

    SCREEN(s, 5, 3, (size_t)10);
    const char *str = "1ABC";
    WRITE(s, str);

    /* Grow */
    ASSERT_TRUE(s.resize(rsz(5, 10)));
    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);

    /* Shrink */
    ASSERT_TRUE(s.resize(rsz(5, 3)));
    EXPECT_DUMP(s, screen, str);
    EXPECT_DUMP(s, viewport, str);

    /* Grow again */
    ASSERT_TRUE(s.resize(rsz(5, 10)));
    EXPECT_DUMP(s, viewport, str);
    EXPECT_DUMP(s, screen, str);
}

TEST(screen, Screen__resize_less_cols_to_eliminate_wide_char) {

    SCREEN(s, 2, 1, (size_t)0);
    const char *str = "\xF0\x9F\x98\x80";
    WRITE(s, str);
    EXPECT_DUMP(s, screen, str);
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
    }

    /* Resize to 1 column can't fit a wide char. So it should be deleted. */
    ASSERT_TRUE(s.resize(rsz(1, 1)));
    EXPECT_DUMP(s, screen, "");
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(0 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::narrow == cell->wide());
    }
}

TEST(screen, Screen__resize_less_cols_to_wrap_wide_char) {

    SCREEN(s, 3, 3, (size_t)0);
    const char *str = "x\xF0\x9F\x98\x80";
    WRITE(s, str);
    EXPECT_DUMP(s, screen, str);
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    ASSERT_TRUE(s.resize(rsz(2, 3)));
    EXPECT_DUMP(s, screen, "x\n\xF0\x9F\x98\x80");
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
        ASSERT_TRUE(list_cell.row->wrap());
    }
}

TEST(screen, Screen__resize_less_cols_to_eliminate_wide_char_with_row_space) {

    SCREEN(s, 2, 2, (size_t)0);
    const char *str = "\xF0\x9F\x98\x80";
    WRITE(s, str);
    EXPECT_DUMP(s, screen, str);
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    ASSERT_TRUE(s.resize(rsz(1, 2)));
    EXPECT_DUMP(s, screen, "");
}

TEST(screen, Screen__resize_less_cols_reflows_cursor_after_wrapped_text) {
    SCREEN(s, 50, 7, (size_t)0);

    for (size_t _i = 0; _i < (size_t)30; _i++) WRITE(s, "a");

    ASSERT_TRUE(0 == s.cursor.y);
    ASSERT_TRUE(30 == s.cursor.x);

    ASSERT_TRUE(s.resize(rsz(25, 7)));

    ASSERT_TRUE(1 == s.cursor.y);
    ASSERT_TRUE(5 == s.cursor.x);
}

TEST(screen, Screen__resize_less_cols_reflows_cursor_after_empty_cells) {
    SCREEN(s, 10, 3, (size_t)0);

    WRITE(s, "abc");
    s.cursorRight(6);

    ASSERT_TRUE(0 == s.cursor.y);
    ASSERT_TRUE(9 == s.cursor.x);

    ASSERT_TRUE(s.resize(rsz(5, 3)));

    ASSERT_TRUE(1 == s.cursor.y);
    ASSERT_TRUE(4 == s.cursor.x);
}

TEST(screen, Screen__resize_more_cols_with_wide_spacer_head) {

    SCREEN(s, 3, 2, (size_t)0);
    const char *str = "  \xF0\x9F\x98\x80";
    WRITE(s, str);
    EXPECT_DUMP(s, screen, "  \n\xF0\x9F\x98\x80");

    /* So this is the key point: we end up with a wide spacer head at
     * the end of row 1, then the emoji, then a wide spacer tail on row 2.
     * We should expect that if we resize to more cols, the wide spacer
     * head is replaced with the emoji. */
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    ASSERT_TRUE(s.resize(rsz(4, 2)));
    EXPECT_DUMP(s, screen, str);
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(3, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(screen, Screen__resize_more_cols_with_wide_spacer_head_multiple_lines) {

    SCREEN(s, 3, 3, (size_t)0);
    const char *str = "xxxyy\xF0\x9F\x98\x80";
    WRITE(s, str);
    EXPECT_DUMP(s, screen, "xxx\nyy\n\xF0\x9F\x98\x80");

    /* Similar to the "wide spacer head" test, but this time we'er going
     * to increase our columns such that multiple rows are unwrapped. */
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(2, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 2)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 2)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    ASSERT_TRUE(s.resize(rsz(8, 2)));
    EXPECT_DUMP(s, screen, str);
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(5, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(6, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(screen, Screen__resize_more_cols_requiring_a_wide_spacer_head) {

    SCREEN(s, 2, 2, (size_t)0);
    const char *str = "xx\xF0\x9F\x98\x80";
    WRITE(s, str);
    EXPECT_DUMP(s, screen, "xx\n\xF0\x9F\x98\x80");
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }

    /* This resizes to 3 columns, which isn't enough space for our wide
     * char to enter row 1. But we need to mark the wide spacer head on the
     * end of the first row since we're wrapping to the next row. */
    ASSERT_TRUE(s.resize(rsz(3, 2)));
    EXPECT_DUMP(s, screen, "xx\n\xF0\x9F\x98\x80");
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(2, 0)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_head == cell->wide());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(0, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        ASSERT_TRUE(0x1F600 == cell->contentCodepoint());
    }
    {
        const PageList::Cell list_cell = s.pages.getCell(Point::screen(1, 1)).value;
        const Cell *cell = list_cell.cell;
        ASSERT_TRUE(Cell::Wide::spacer_tail == cell->wide());
    }
}

TEST(screen, Screen__resize_more_cols_with_cursor_at_prompt) {

    SCREEN(s, 10, 3, (size_t)5);

    /* zig fmt: off */
    WRITE(s, "ABCDE\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_eol));
    WRITE(s, "echo");
    /* zig fmt: on */

    EXPECT_DUMP(s, viewport, "ABCDE\n> echo");

    {
        Screen::Resize r = rsz(20, 3);
        r.prompt_redraw = terminal::osc::semantic_prompt::Redraw::true_;
        ASSERT_TRUE(s.resize(r));
    }

    /* Cursor should not move */
    ASSERT_TRUE(6 == s.cursor.x);
    ASSERT_TRUE(1 == s.cursor.y);

    EXPECT_DUMP(s, viewport, "ABCDE");
}

TEST(screen, Screen__resize_more_cols_with_cursor_not_at_prompt) {

    SCREEN(s, 10, 3, (size_t)5);

    /* zig fmt: off */
    WRITE(s, "ABCDE\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_eol));
    WRITE(s, "echo\n");
    WRITE(s, "output");
    /* zig fmt: on */

    EXPECT_DUMP(s, viewport, "ABCDE\n> echo\noutput");

    {
        Screen::Resize r = rsz(20, 3);
        r.prompt_redraw = terminal::osc::semantic_prompt::Redraw::true_;
        ASSERT_TRUE(s.resize(r));
    }

    /* Cursor should not move */
    ASSERT_TRUE(6 == s.cursor.x);
    ASSERT_TRUE(2 == s.cursor.y);

    EXPECT_DUMP(s, viewport, "ABCDE\n> echo\noutput");
}

TEST(screen, Screen__resize_with_prompt_redraw_last_clears_only_one_line) {

    SCREEN(s, 10, 4, (size_t)5);

    /* zig fmt: off */
    WRITE(s, "ABCDE\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello\n");
    WRITE(s, "world");
    /* zig fmt: on */

    EXPECT_DUMP(s, viewport, "ABCDE\n> hello\nworld");

    /* Cursor is at end of "world" line with semantic_content = .input */
    {
        Screen::Resize r = rsz(20, 4);
        r.prompt_redraw = terminal::osc::semantic_prompt::Redraw::last;
        ASSERT_TRUE(s.resize(r));
    }

    /* With .last, only the current line where cursor is should be cleared */
    EXPECT_DUMP(s, viewport, "ABCDE\n> hello");
}

TEST(screen, Screen__resize_with_prompt_redraw_last_multiline_prompt_clears_only_last_line) {

    SCREEN(s, 20, 5, (size_t)5);

    /* Create a 3-line prompt: 1 initial + 2 continuation lines
     * zig fmt: off */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "line1\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::continuation));
    WRITE(s, "line2\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::continuation));
    WRITE(s, "line3");
    /* zig fmt: on */

    EXPECT_DUMP(s, viewport, "line1\nline2\nline3");

    /* Cursor is at end of line3 (the last continuation line) */
    {
        Screen::Resize r = rsz(30, 5);
        r.prompt_redraw = terminal::osc::semantic_prompt::Redraw::last;
        ASSERT_TRUE(s.resize(r));
    }

    /* With .last, only line3 (where cursor is) should be cleared */
    EXPECT_DUMP(s, viewport, "line1\nline2");
}

TEST(screen, Screen__select_untracked) {

    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "ABC  DEF\n 123\n456");

    ASSERT_TRUE(!s.selection.has);
    const size_t tracked = s.pages.countTrackedPins();
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(0, 0)), pinAt(s, Point::active(3, 0)), false)));
    ASSERT_TRUE(tracked + 2 == s.pages.countTrackedPins());
    ASSERT_TRUE(s.select(Maybe<Selection>()));
    ASSERT_TRUE(tracked == s.pages.countTrackedPins());
}

TEST(screen, Screen__select_replaces_existing_pins) {

    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "ABC  DEF\n 123\n456");

    const size_t tracked = s.pages.countTrackedPins();
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(0, 0)), pinAt(s, Point::active(3, 0)), false)));
    ASSERT_TRUE(tracked + 2 == s.pages.countTrackedPins());

    /* Replacing the selection must untrack the prior selection's pins
     * rather than leak them. */
    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(0, 1)), pinAt(s, Point::active(2, 1)), false)));
    ASSERT_TRUE(tracked + 2 == s.pages.countTrackedPins());
}

TEST(screen, Screen__reselecting_tracked_selection_preserves_its_pins) {

    SCREEN(s, 10, 2, (size_t)0);

    ASSERT_TRUE(s.select(Selection::init(pinAt(s, Point::active(1, 0)), pinAt(s, Point::active(3, 0)), false)));

    ASSERT_TRUE(s.select(s.selection.value));
    ASSERT_TRUE(Selection::Order::forward == s.selection.value.order(&s));
}

TEST(screen, Screen__selectAll) {

    SCREEN(s, 10, 10, (size_t)0);

    {
        WRITE(s, "ABC  DEF\n 123\n456");
        SEL(sel, s, s.selectAll());
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 2)));
    }

    {
        WRITE(s, "\nFOO\n BAR\n BAZ\n QWERTY\n 12345678");
        SEL(sel, s, s.selectAll());
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(8, 7)));
    }
}

TEST(screen, Screen__selectLine) {

    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "ABC  DEF\n 123\n456");

    /* Outside of active area
     * try testing.expect(s.selectLine(.{ .x = 13, .y = 0 }) == null);
     * try testing.expect(s.selectLine(.{ .x = 0, .y = 5 }) == null); */

    /* Going forward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }

    /* Going backward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(7, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }

    /* Going forward and backward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(3, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }

    /* Outside active area */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(9, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }
}

TEST(screen, Screen__selectLine_across_soft_wrap) {

    SCREEN(s, 5, 10, (size_t)0);
    WRITE(s, " 12 34012   \n 123");

    /* Going forward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(3, 1)));
    }
}

TEST(screen, Screen__selectLine_across_full_soft_wrap) {

    SCREEN(s, 5, 5, (size_t)0);
    WRITE(s, "1ABCD2EFGH\n3IJKL");

    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(2, 1)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 1)));
    }
}

TEST(screen, Screen__selectLine_across_soft_wrap_ignores_blank_lines) {

    SCREEN(s, 5, 10, (size_t)0);
    WRITE(s, " 12 34012             \n 123");

    /* Going forward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(3, 1)));
    }

    /* Going backward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 1)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(3, 1)));
    }

    /* Going forward and backward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(3, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(3, 1)));
    }
}

TEST(screen, Screen__selectLine_disabled_whitespace_trimming) {

    SCREEN(s, 5, 10, (size_t)0);
    WRITE(s, " 12 34012   \n 123");

    /* Going forward */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 0)), true)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 2)));
    }

    /* Non-wrapped */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 3)), true)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 3)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 3)));
    }
}

TEST(screen, Screen__selectLine_with_scrollback) {

    SCREEN(s, 2, 3, (size_t)5);
    WRITE(s, "1A\n2B\n3C\n4D\n5E");

    /* Selecting first line */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(1, 0)));
    }

    /* Selecting last line */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 2)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 2)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(1, 2)));
    }
}

/* https://github.com/mitchellh/ghostty/issues/1329 */
TEST(screen, Screen__selectLine_semantic_prompt_boundary) {

    SCREEN(s, 5, 10, (size_t)0);
    WRITE(s, "ABCDE\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "A    ");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "> ");

    EXPECT_DUMP(s, screen, "ABCDE\nA    \n> ");

    /* Selecting output stops at the prompt even if soft-wrapped */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 1)))));
        EXPECT_STR("A", s.selectionString(Screen::SelectionString(sel, false)));
    }
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 2)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 2)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(0, 2)));
    }
}

TEST(screen, Screen__selectLine_semantic_prompt_to_input_boundary) {

    SCREEN(s, 10, 5, (size_t)0);

    /* Write prompt followed by user input on same row: "$>command"
     * Using non-whitespace to avoid whitespace trimming affecting the test */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "$>");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "command");

    /* Selecting from prompt should only select prompt */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(1, 0)));
    }

    /* Selecting from input should only select input */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(5, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(2, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(8, 0)));
    }
}

TEST(screen, Screen__selectLine_semantic_input_to_output_boundary) {

    SCREEN(s, 10, 5, (size_t)0);

    /* Row 0: user input */
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "ls -la\n");
    /* Row 1: command output */
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "file.txt");

    /* Selecting from input should only select input */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(2, 0)))));
        EXPECT_STR("ls -la", s.selectionString(Screen::SelectionString(sel, false)));
    }

    /* Selecting from output should only select output */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(2, 1)))));
        EXPECT_STR("file.txt", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__selectLine_semantic_mid_row_boundary) {

    SCREEN(s, 10, 5, (size_t)0);

    /* Single row with output then prompt then input: "out$>cmd"
     * Using non-whitespace to avoid whitespace trimming affecting the test */
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "out");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "$>");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "cmd");

    /* Selecting from output should stop at prompt */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(1, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(2, 0)));
    }

    /* Selecting from prompt should only select prompt */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(3, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(3, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(4, 0)));
    }

    /* Selecting from input should only select input */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(6, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(5, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(7, 0)));
    }
}

TEST(screen, Screen__selectLine_semantic_boundary_soft_wrap_with_mid_row_transition) {

    SCREEN(s, 5, 5, (size_t)0);

    /* Row 0: prompt "$ " + input "cmd" (soft-wraps)
     * Row 1: input continues "12" + output "out" */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "$ ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "cmd12");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "out");

    /* Verify layout */
    EXPECT_DUMP(s, screen, "$ cmd\n12out");

    /* Selecting from input on row 0 should get all input across soft-wrap */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(3, 0)))));
        EXPECT_STR("cmd12", s.selectionString(Screen::SelectionString(sel, false)));
    }

    /* Selecting from input on row 1 should get all input across soft-wrap */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 1)))));
        EXPECT_STR("cmd12", s.selectionString(Screen::SelectionString(sel, false)));
    }

    /* Selecting from output should only get output */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(3, 1)))));
        EXPECT_STR("out", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__selectLine_semantic_boundary_disabled) {

    SCREEN(s, 10, 5, (size_t)0);

    /* Write prompt followed by input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "$ ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "command");

    /* With semantic_prompt_boundary = false, should select entire line */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 0)), false, false)));
        EXPECT_STR("$ command", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__selectLine_semantic_boundary_first_cell_of_row) {

    SCREEN(s, 5, 5, (size_t)0);

    /* Row 0: input that soft-wraps
     * Row 1: output starts at first cell */
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "12345");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "ABCDE");

    /* Verify soft-wrap happened */
    {
        const Pin pin = pinAt(s, Point::active(0, 0));
        Row *row = pin.rowAndCell().row;
        ASSERT_TRUE(row->wrap());
    }

    /* Selecting from input should stop before output on row 1 */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(2, 0)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(4, 0)));
    }

    /* Selecting from output should only get output */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(2, 1)))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(4, 1)));
    }
}

TEST(screen, Screen__selectLine_semantic_boundary_across_mixed_width_pages) {

    SCREEN(s, 4, 2, (size_t)0);

    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "ABCD");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "E");

    PageList::Node *first = s.pages.pages.first;
    ASSERT_TRUE(s.pages.split(Pin(first, 1, 0)) == PageList::SplitError::none);
    PageList::Node *second = first->next;
    first->page()->size.cols = 2;
    s.pages.pauseIntegrityChecks(true);
    PauseGuard pause_guard = {&s.pages};

    ASSERT_TRUE(2 == first->cols());
    ASSERT_TRUE(4 == second->cols());

    SEL(sel, s, s.selectLine(selLine(Pin(first, 0, 1))));
    ASSERT_TRUE(Pin(first, 0, 0).eql(sel.start()));
    ASSERT_TRUE(Pin(first, 0, 1).eql(sel.end()));
}

TEST(screen, Screen__selectLine_semantic_all_same_content) {

    SCREEN(s, 5, 5, (size_t)0);

    /* All prompt content that soft-wraps */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "prompt text");

    /* Verify soft-wrap */
    EXPECT_DUMP(s, screen, "promp\nt tex\nt");

    /* Should select all prompt content across soft-wraps */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(2, 1)))));
        EXPECT_STR("prompt text", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__selectWord) {

    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "ABC  DEF\n 123\n456");

    /* Default boundary codepoints for word selection */
    static const uint32_t boundary_codepoints[] = {0,   ' ', '\t', '\'', '"', 0x2502, '`', '|', ':',  ';',  ',', '(', ')', '[',  ']',  '{', '}', '<', '>',  '$'};
    const size_t boundary_codepoints_len = sizeof(boundary_codepoints) / sizeof(boundary_codepoints[0]);

    /* Outside of active area
     * try testing.expect(s.selectWord(.{ .x = 9, .y = 0 }) == null);
     * try testing.expect(s.selectWord(.{ .x = 0, .y = 5 }) == null); */

    /* Going forward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(0, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 0)));
    }

    /* Going backward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(2, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 0)));
    }

    /* Going forward and backward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 0)));
    }

    /* Whitespace */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(3, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(3, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 0)));
    }

    /* Whitespace single char */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(0, 1)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 1)));
    }

    /* End of screen */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 2)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 2)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 2)));
    }
}

TEST(screen, Screen__selectWord_across_soft_wrap) {

    SCREEN(s, 5, 10, (size_t)0);
    WRITE(s, " 1234012\n 123");

    /* Default boundary codepoints for word selection */
    static const uint32_t boundary_codepoints[] = {0,   ' ', '\t', '\'', '"', 0x2502, '`', '|', ':',  ';',  ',', '(', ')', '[',  ']',  '{', '}', '<', '>',  '$'};
    const size_t boundary_codepoints_len = sizeof(boundary_codepoints) / sizeof(boundary_codepoints[0]);

    EXPECT_DUMP(s, screen, " 1234\n012\n 123");

    /* Going forward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 1)));
    }

    /* Going backward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 1)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 1)));
    }

    /* Going forward and backward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(3, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 1)));
    }
}

TEST(screen, Screen__selectWord_whitespace_across_soft_wrap) {

    SCREEN(s, 5, 10, (size_t)0);
    WRITE(s, "1       1\n 123");

    /* Default boundary codepoints for word selection */
    static const uint32_t boundary_codepoints[] = {0,   ' ', '\t', '\'', '"', 0x2502, '`', '|', ':',  ';',  ',', '(', ')', '[',  ']',  '{', '}', '<', '>',  '$'};
    const size_t boundary_codepoints_len = sizeof(boundary_codepoints) / sizeof(boundary_codepoints[0]);

    /* Going forward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 1)));
    }

    /* Going backward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 1)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 1)));
    }

    /* Going forward and backward */
    {
        SEL(sel, s, s.selectWord(pinAt(s, Point::active(3, 0)), boundary_codepoints, boundary_codepoints_len));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 1)));
    }
}

TEST(screen, Screen__selectWord_with_character_boundary) {

    /* Default boundary codepoints for word selection */
    static const uint32_t boundary_codepoints[] = {0,   ' ', '\t', '\'', '"', 0x2502, '`', '|', ':',  ';',  ',', '(', ')', '[',  ']',  '{', '}', '<', '>',  '$'};
    const size_t boundary_codepoints_len = sizeof(boundary_codepoints) / sizeof(boundary_codepoints[0]);

    static const char *const cases[] = {
        " 'abc' \n123",
        " \"abc\" \n123",
        " \xE2\x94\x82" "abc\xE2\x94\x82 \n123",
        " `abc` \n123",
        " |abc| \n123",
        " :abc: \n123",
        " ;abc; \n123",
        " ,abc, \n123",
        " (abc( \n123", " )abc) \n123",
        " [abc[ \n123",
        " ]abc] \n123",
        " {abc{ \n123",
        " }abc} \n123",
        " <abc< \n123",
        " >abc> \n123",
        " $abc$ \n123",
    };

    for (size_t case_i = 0; case_i < sizeof(cases) / sizeof(cases[0]); case_i++) {
        const char *case_ = cases[case_i];
        SCREEN(s, 20, 10, (size_t)0);
        WRITE(s, case_);

        /* Inside character forward */
        {
            SEL(sel, s, s.selectWord(pinAt(s, Point::active(2, 0)), boundary_codepoints, boundary_codepoints_len));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(2, 0)));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 0)));
        }

        /* Inside character backward */
        {
            SEL(sel, s, s.selectWord(pinAt(s, Point::active(4, 0)), boundary_codepoints, boundary_codepoints_len));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(2, 0)));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 0)));
        }

        /* Inside character bidirectional */
        {
            SEL(sel, s, s.selectWord(pinAt(s, Point::active(3, 0)), boundary_codepoints, boundary_codepoints_len));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(2, 0)));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 0)));
        }

        /* On quote
         * NOTE: this behavior is not ideal, so we can change this one day,
         * but I think its also not that important compared to the above. */
        {
            SEL(sel, s, s.selectWord(pinAt(s, Point::active(1, 0)), boundary_codepoints, boundary_codepoints_len));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 0)));
            ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(1, 0)));
        }
    }
}

TEST(screen, Screen__selectOutput) {

    SCREEN(s, 10, 15, (size_t)0);

    /* Build content with cell-level semantic content:
     * Row 0-1: output1 (output)
     * Row 2: prompt2 (prompt)
     * Row 3: input2 (input)
     * Row 4-7: output2 (output, with overflow causing wrap)
     * Row 8: "$ " (prompt) + "input3" (input)
     * Row 9-11: output3 (output) */
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "output1\n");
    WRITE(s, "output1\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "prompt2\n");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "input2\n");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "output2output2output2output2\n");
    WRITE(s, "output2\n");
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "$ ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "input3\n");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "output3\n");
    WRITE(s, "output3\n");
    WRITE(s, "output3");

    /* First output block (rows 0-1), should select those rows */
    {
        SEL(sel, s, s.selectOutput(pinAt(s, Point::active(1, 1))));
        EXPECT_STR("output1\noutput1", s.selectionString(Screen::SelectionString(sel, false)));
    }
    /* Second output block (rows 4-7) */
    {
        SEL(sel, s, s.selectOutput(pinAt(s, Point::active(3, 7))));
        EXPECT_STR("output2output2output2output2\noutput2", s.selectionString(Screen::SelectionString(sel, false)));
    }
    /* Third output block (rows 9-11) */
    {
        SEL(sel, s, s.selectOutput(pinAt(s, Point::active(2, 10))));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.start()), Point::active(0, 9)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::active, sel.end()), Point::active(6, 11)));
    }
    /* Click on prompt should return null */
    {
        ASSERT_TRUE(!s.selectOutput(pinAt(s, Point::active(1, 8))).has);
    }
    /* Click on input should return null */
    {
        ASSERT_TRUE(!s.selectOutput(pinAt(s, Point::active(5, 8))).has);
    }
}

TEST(screen, Screen__selectionString_basic) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 1)), pinAt(s, Point::screen(2, 2)), false);
        EXPECT_STR("2EFGH\n3IJ", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString_start_outside_of_written_area) {

    SCREEN(s, 5, 10, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 5)), pinAt(s, Point::screen(2, 6)), false);
        EXPECT_STR("", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString_end_outside_of_written_area) {

    SCREEN(s, 5, 10, (size_t)0);
    const char *str = "1ABCD\n2EFGH\n3IJKL";
    WRITE(s, str);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 2)), pinAt(s, Point::screen(2, 6)), false);
        EXPECT_STR("3IJKL", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString_trim_space) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1AB  \n2EFGH\n3IJKL";
    WRITE(s, str);

    const Selection sel = Selection::init(pinAt(s, Point::screen(0, 0)), pinAt(s, Point::screen(2, 1)), false);

    {
        EXPECT_STR("1AB\n2EF", s.selectionString(Screen::SelectionString(sel, true)));
    }

    /* No trim */
    {
        EXPECT_STR("1AB  \n2EF", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__selectionString_trim_empty_line) {

    SCREEN(s, 5, 5, (size_t)0);
    const char *str = "1AB  \n\n2EFGH\n3IJKL";
    WRITE(s, str);

    const Selection sel = Selection::init(pinAt(s, Point::screen(0, 0)), pinAt(s, Point::screen(2, 2)), false);

    {
        EXPECT_STR("1AB\n\n2EF", s.selectionString(Screen::SelectionString(sel, true)));
    }

    /* No trim */
    {
        EXPECT_STR("1AB  \n\n2EF", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__selectionString_soft_wrap) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABCD2EFGH3IJKL";
    WRITE(s, str);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 1)), pinAt(s, Point::screen(2, 2)), false);
        EXPECT_STR("2EFGH3IJ", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString_wide_char) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1A\xE2\x9A\xA1";
    WRITE(s, str);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 0)), pinAt(s, Point::screen(3, 0)), false);
        EXPECT_STR(str, s.selectionString(Screen::SelectionString(sel, true)));
    }

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 0)), pinAt(s, Point::screen(2, 0)), false);
        EXPECT_STR(str, s.selectionString(Screen::SelectionString(sel, true)));
    }

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(3, 0)), pinAt(s, Point::screen(3, 0)), false);
        EXPECT_STR("\xE2\x9A\xA1", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString_wide_char_with_header) {

    SCREEN(s, 5, 3, (size_t)0);
    const char *str = "1ABC\xE2\x9A\xA1";
    WRITE(s, str);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 0)), pinAt(s, Point::screen(4, 0)), false);
        EXPECT_STR(str, s.selectionString(Screen::SelectionString(sel, true)));
    }
}

/* https://github.com/mitchellh/ghostty/issues/289 */
TEST(screen, Screen__selectionString_empty_with_soft_wrap) {

    SCREEN(s, 5, 2, (size_t)0);

    /* Let me describe the situation that caused this because this
     * test is not obvious. By writing an emoji below, we introduce
     * one cell with the emoji and one cell as a "wide char spacer".
     * We then soft wrap the line by writing spaces.
     *
     * By selecting only the tail, we'd select nothing and we had
     * a logic error that would cause a crash. */
    WRITE(s, "\xF0\x9F\x91\xA8");
    WRITE(s, "      ");

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(1, 0)), pinAt(s, Point::screen(2, 0)), false);
        EXPECT_STR("\xF0\x9F\x91\xA8", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString_with_zero_width_joiner) {

    SCREEN(s, 10, 1, (size_t)0);
    /* this has a ZWJ */
    const char *str = "\xF0\x9F\x91\xA8\xE2\x80\x8D";
    WRITE(s, str);

    /* Integrity check */
    {
        const Pin pin = pinAt(s, Point::screen(0, 0));
        Cell *cell = pin.rowAndCell().cell;
        ASSERT_TRUE(0x1F468 == cell->contentCodepoint());
        ASSERT_TRUE(Cell::Wide::wide == cell->wide());
        size_t cps_len = 0;
        const uint32_t *cps = pin.node->page()->lookupGrapheme(cell, &cps_len);
        ASSERT_TRUE(cps != nullptr);
        ASSERT_TRUE(1 == cps_len);
    }

    /* The real test */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(0, 0)), pinAt(s, Point::screen(1, 0)), false);
        EXPECT_STR("\xF0\x9F\x91\xA8\xE2\x80\x8D", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__selectionString__rectangle__basic) {

    SCREEN(s, 30, 5, (size_t)0);
    const char *str = "Lorem ipsum dolor\nsit amet, consectetur\nadipiscing elit, sed do\neiusmod tempor incididunt\nut labore et dolore";
    const Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(6, 3)), true);
    const char *expected = "t ame\nipisc\nusmod";
    WRITE(s, str);

    EXPECT_STR(expected, s.selectionString(Screen::SelectionString(sel, true)));
}

TEST(screen, Screen__selectionString__rectangle__w_EOL) {

    SCREEN(s, 30, 5, (size_t)0);
    const char *str = "Lorem ipsum dolor\nsit amet, consectetur\nadipiscing elit, sed do\neiusmod tempor incididunt\nut labore et dolore";
    const Selection sel = Selection::init(pinAt(s, Point::screen(12, 0)), pinAt(s, Point::screen(26, 4)), true);
    const char *expected = "dolor\nnsectetur\nlit, sed do\nor incididunt\n dolore";
    WRITE(s, str);

    EXPECT_STR(expected, s.selectionString(Screen::SelectionString(sel, true)));
}

TEST(screen, Screen__selectionString__rectangle__more_complex_w_breaks) {

    SCREEN(s, 30, 8, (size_t)0);
    const char *str = "Lorem ipsum dolor\nsit amet, consectetur\nadipiscing elit, sed do\neiusmod tempor incididunt\nut labore et dolore\n\nmagna aliqua. Ut enim\nad minim veniam, quis";
    const Selection sel = Selection::init(pinAt(s, Point::screen(11, 2)), pinAt(s, Point::screen(26, 7)), true);
    const char *expected = "elit, sed do\npor incididunt\nt dolore\n\na. Ut enim\nniam, quis";
    WRITE(s, str);

    EXPECT_STR(expected, s.selectionString(Screen::SelectionString(sel, true)));
}

TEST(screen, Screen__selectionString_multi_page) {

    SCREEN(s, 10, 3, (size_t)2048);

    const size_t first_page_size = s.pages.pages.first->capacity().rows;

    /* Lazy way to seek to the first page boundary. */
    s.pages.pages.first->page()->pauseIntegrityChecks(true);
    for (size_t i_ = 0; i_ < (size_t)(first_page_size - 1); i_++) {
        WRITE(s, "\n");
    }
    s.pages.pages.first->page()->pauseIntegrityChecks(false);

    WRITE(s, "123456789\n!@#$%^&*(\n123456789");

    {
        const Selection sel = Selection::init(pinAt(s, Point::active(0, 0)), pinAt(s, Point::active(2, 2)), false);
        EXPECT_STR("123456789\n!@#$%^&*(\n123", s.selectionString(Screen::SelectionString(sel, true)));
    }
}

TEST(screen, Screen__lineIterator) {
    SCREEN(s, 5, 5, (size_t)0);
    const char *str = "1ABCD\n2EFGH";
    WRITE(s, str);

    /* Test the line iterator */
    Screen::LineIterator iter = s.lineIterator(pinAt(s, Point::viewport()));
    {
        Selection sel;
        ASSERT_TRUE(iter.next(&sel));
        EXPECT_STR("1ABCD", s.selectionString(Screen::SelectionString(sel, false)));
    }
    {
        Selection sel;
        ASSERT_TRUE(iter.next(&sel));
        EXPECT_STR("2EFGH", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

TEST(screen, Screen__lineIterator_soft_wrap) {
    SCREEN(s, 5, 5, (size_t)0);
    const char *str = "1ABCD2EFGH\n3ABCD";
    WRITE(s, str);

    /* Test the line iterator */
    Screen::LineIterator iter = s.lineIterator(pinAt(s, Point::viewport()));
    {
        Selection sel;
        ASSERT_TRUE(iter.next(&sel));
        EXPECT_STR("1ABCD2EFGH", s.selectionString(Screen::SelectionString(sel, false)));
    }
    {
        Selection sel;
        ASSERT_TRUE(iter.next(&sel));
        EXPECT_STR("3ABCD", s.selectionString(Screen::SelectionString(sel, false)));
    }
    /* try testing.expect(iter.next() == null); */
}

TEST(screen, Screen__hyperlink_start_end) {
    SCREEN(s, 5, 5, (size_t)0);

    ASSERT_TRUE(s.cursor.hyperlink_id == 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }

    NOERR(startLink(s, "http://example.com"));
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }

    s.endHyperlink();
    ASSERT_TRUE(s.cursor.hyperlink_id == 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }
}

TEST(screen, Screen__hyperlink_accepts_its_current_values) {
    SCREEN(s, 5, 5, (size_t)0);

    NOERR(startLink(s, "http://example.com", "current"));
    const hyperlink::Hyperlink *current = s.cursor.hyperlink;
    NOERR(s.startHyperlink(current->uri, current->uri_len, current->id.explicit_ptr, current->id.explicit_len));

    EXPECT_STR("http://example.com", std::string((const char *)s.cursor.hyperlink->uri, s.cursor.hyperlink->uri_len));
    EXPECT_STR("current", std::string((const char *)s.cursor.hyperlink->id.explicit_ptr,
                                      s.cursor.hyperlink->id.explicit_len));
}

TEST(screen, Screen__implicit_hyperlink_ID_wraps) {
    SCREEN(s, 5, 5, (size_t)0);

    s.cursor.hyperlink_implicit_id = UINT32_MAX;
    NOERR(startLink(s, "http://example.com"));

    ASSERT_TRUE((size::OffsetInt)0 == s.cursor.hyperlink_implicit_id);
    ASSERT_TRUE((size::OffsetInt)UINT32_MAX == s.cursor.hyperlink->id.implicit);

    /* A failed allocation must roll the wrapped counter back to its
     * original value as well. */
    s.endHyperlink();
    s.cursor.hyperlink_implicit_id = UINT32_MAX;
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    failing.fail_index = failing.alloc_index;
    {
        const zigstd::Allocator original_alloc = s.alloc;
        s.alloc = failing.allocator();
        const PageList::IncreaseCapacityError e = startLink(s, "http://example.com");
        s.alloc = original_alloc;
        ASSERT_TRUE(e == PageList::IncreaseCapacityError::OutOfMemory);
    }
    ASSERT_TRUE((size::OffsetInt)UINT32_MAX == s.cursor.hyperlink_implicit_id);
}

TEST(screen, Screen__hyperlink_reuse) {
    SCREEN(s, 5, 5, (size_t)0);

    ASSERT_TRUE(s.cursor.hyperlink_id == 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }

    /* Use it for the first time */
    NOERR(startLink(s, "http://example.com"));
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    const hyperlink::Id id = s.cursor.hyperlink_id;

    /* Reuse the same hyperlink, expect we have the same ID */
    NOERR(startLink(s, "http://example.com"));
    ASSERT_TRUE(id == s.cursor.hyperlink_id);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }

    s.endHyperlink();
    ASSERT_TRUE(s.cursor.hyperlink_id == 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }
}

TEST(screen, Screen__hyperlink_cursor_state_on_resize) {
    /* This test depends on underlying PageList implementation so
     * it may be invalid one day. It's here to document/verify the
     * current behavior. */
    SCREEN(s, 5, 10, (size_t)0);

    /* Start a hyperlink */
    NOERR(startLink(s, "http://example.com"));
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }

    /* Resize. Any column growth will trigger a page to be reallocated. */
    ASSERT_TRUE(s.resize(rsz(10, 10)));
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }

    s.endHyperlink();
    ASSERT_TRUE(s.cursor.hyperlink_id == 0);
    {
        Page *page = s.cursor.page_pin->node->page();
        ASSERT_TRUE(0 == page->hyperlink_set.count());
    }
}

TEST(screen, Screen__cursorSetHyperlink_OOM___URI_too_large_for_string_alloc) {
    SCREEN(s, 80, 24, (size_t)0);

    /* Start a hyperlink with a URI that just barely fits in the string alloc.
     * This will ensure that additional string alloc space is needed for the
     * redundant copy of the URI when the page is re-alloced. */
    const std::string uri(page::std_capacity().string_bytes - 8, 'a');
    NOERR(s.startHyperlink((const uint8_t *)uri.data(), uri.size(), nullptr, 0));

    /* Figure out how many cells should can have hyperlinks in this page,
     * and write twice that number, to guarantee the capacity needs to be
     * increased at some point. */
    const size_t base_capacity = s.cursor.page_pin->node->page()->hyperlinkCapacity();
    const size_t base_string_bytes = s.cursor.page_pin->node->capacity().string_bytes;
    for (size_t i = 0; i < base_capacity * 2; i++) {
        NOERR(s.cursorSetHyperlink());
        if (s.cursor.x >= s.pages.cols - 1) {
            ASSERT_TRUE(s.cursorDownOrScroll());
            s.cursorHorizontalAbsolute(0);
        } else {
            s.cursorRight(1);
        }
    }

    /* Make sure the capacity really did increase. */
    ASSERT_TRUE(base_capacity < s.cursor.page_pin->node->page()->hyperlinkCapacity());
    /* And that our string_bytes increased as well. */
    ASSERT_TRUE(base_string_bytes < s.cursor.page_pin->node->capacity().string_bytes);
}

TEST(screen, Screen__increaseCapacity_cursor_style_ref_count_preserved) {
    SCREEN(s, 5, 5, (size_t)0);
    NOERR(s.setAttribute(attr(A::bold)));
    WRITE(s, "1ABCD");

    /* We should have one page and it should be our cursor page */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    ASSERT_TRUE(s.pages.pages.first == s.cursor.page_pin->node);

    const style::Style old_style = s.cursor.style;

    {
        Page *page = s.pages.pages.last->page();
        /* 5 chars + cursor = 6 refs */
        ASSERT_TRUE(6 == page->styles.refCount((const void *)page->memory, s.cursor.style_id));
    }

    /* This forces the page to change via increaseCapacity. */
    PageList::Node *new_node = nullptr;
    NOERR(s.increaseCapacity(s.cursor.page_pin->node, PageList::IncreaseCapacity::grapheme_bytes, &new_node));

    /* Cursor's page_pin should now point to the new node */
    ASSERT_TRUE(s.cursor.page_pin->node == new_node);

    /* Verify cursor's page_cell and page_row are correctly reloaded from the pin */
    const Page::RowAndCell page_rac = s.cursor.page_pin->rowAndCell();
    ASSERT_TRUE(s.cursor.page_row == page_rac.row);
    ASSERT_TRUE(s.cursor.page_cell == page_rac.cell);

    /* Style should be preserved */
    ASSERT_TRUE(old_style.eql(s.cursor.style));
    ASSERT_TRUE(s.cursor.style_id != style::default_id);

    /* After increaseCapacity, the 5 chars are cloned (5 refs) and
     * the cursor's style is re-added (1 ref) = 6 total. */
    {
        Page *page = s.pages.pages.last->page();
        const size_t ref_count = page->styles.refCount((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(6 == ref_count);
    }
}

TEST(screen, Screen__increaseCapacity_cursor_hyperlink_ref_count_preserved) {
    SCREEN(s, 5, 5, (size_t)0);
    NOERR(startLink(s, "https://example.com/"));
    WRITE(s, "1ABCD");

    /* We should have one page and it should be our cursor page */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    ASSERT_TRUE(s.pages.pages.first == s.cursor.page_pin->node);

    {
        Page *page = s.pages.pages.last->page();
        /* Cursor has the hyperlink active = 1 count in hyperlink_set */
        ASSERT_TRUE(1 == page->hyperlink_set.count());
        ASSERT_TRUE(s.cursor.hyperlink_id != 0);
        ASSERT_TRUE(s.cursor.hyperlink != nullptr);
    }

    /* This forces the page to change via increaseCapacity. */
    PageList::Node *n = nullptr;
    NOERR(s.increaseCapacity(s.cursor.page_pin->node, PageList::IncreaseCapacity::grapheme_bytes, &n));

    /* Hyperlink should be preserved with correct URI */
    ASSERT_TRUE(s.cursor.hyperlink != nullptr);
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    EXPECT_STR("https://example.com/", std::string((const char *)s.cursor.hyperlink->uri, s.cursor.hyperlink->uri_len));

    /* After increaseCapacity, the hyperlink is re-added to the new page. */
    {
        Page *page = s.pages.pages.last->page();
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }
}

TEST(screen, Screen__increaseCapacity_cursor_with_both_style_and_hyperlink_preserved) {
    SCREEN(s, 5, 5, (size_t)0);

    NOERR(s.setAttribute(attr(A::bold)));
    NOERR(startLink(s, "https://example.com/"));
    WRITE(s, "1ABCD");

    /* We should have one page and it should be our cursor page */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    ASSERT_TRUE(s.pages.pages.first == s.cursor.page_pin->node);

    const style::Style old_style = s.cursor.style;

    {
        Page *page = s.pages.pages.last->page();
        /* 5 chars + cursor = 6 refs for style */
        ASSERT_TRUE(6 == page->styles.refCount((const void *)page->memory, s.cursor.style_id));
        /* Cursor has the hyperlink active = 1 count in hyperlink_set */
        ASSERT_TRUE(1 == page->hyperlink_set.count());
        ASSERT_TRUE(s.cursor.style_id != style::default_id);
        ASSERT_TRUE(s.cursor.hyperlink_id != 0);
        ASSERT_TRUE(s.cursor.hyperlink != nullptr);
    }

    /* This forces the page to change via increaseCapacity. */
    PageList::Node *n = nullptr;
    NOERR(s.increaseCapacity(s.cursor.page_pin->node, PageList::IncreaseCapacity::grapheme_bytes, &n));

    /* Style should be preserved */
    ASSERT_TRUE(old_style.eql(s.cursor.style));
    ASSERT_TRUE(s.cursor.style_id != style::default_id);

    /* Hyperlink should be preserved */
    ASSERT_TRUE(s.cursor.hyperlink != nullptr);
    ASSERT_TRUE(s.cursor.hyperlink_id != 0);
    EXPECT_STR("https://example.com/", std::string((const char *)s.cursor.hyperlink->uri, s.cursor.hyperlink->uri_len));

    /* After increaseCapacity, both style and hyperlink are re-added to the new page. */
    {
        Page *page = s.pages.pages.last->page();
        const size_t ref_count = page->styles.refCount((const void *)page->memory, s.cursor.style_id);
        ASSERT_TRUE(6 == ref_count);
        ASSERT_TRUE(1 == page->hyperlink_set.count());
    }
}

TEST(screen, Screen__increaseCapacity_non_cursor_page_returns_early) {
    /* Test that calling increaseCapacity on a page that is NOT the cursor's
     * page properly delegates to pages.increaseCapacity without doing the
     * extra cursor accounting (style/hyperlink re-adding). */
    SCREEN(s, 80, 24, (size_t)10000);

    /* Set up a custom style and hyperlink on the cursor */
    NOERR(s.setAttribute(attr(A::bold)));
    NOERR(startLink(s, "https://example.com/"));
    WRITE(s, "Hello");

    /* Store cursor state before growing pages */
    const style::Style old_style = s.cursor.style;
    const style::Id old_style_id = s.cursor.style_id;
    const hyperlink::Hyperlink *old_hyperlink = s.cursor.hyperlink;
    const hyperlink::Id old_hyperlink_id = s.cursor.hyperlink_id;

    /* The cursor is on the first (and only) page */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);

    /* Grow pages until we have multiple pages. The cursor's pin stays on
     * the first page since we're just adding rows. */
    PageList::Node *first_page_node = s.pages.pages.first;
    first_page_node->page()->pauseIntegrityChecks(true);
    const size_t grow_n = (size_t)(first_page_node->capacity().rows - first_page_node->rows());
    for (size_t i = 0; i < grow_n; i++) {
        PageList::Node *g;
        ASSERT_TRUE(s.pages.grow(&g));
    }
    first_page_node->page()->pauseIntegrityChecks(false);
    {
        PageList::Node *g;
        ASSERT_TRUE(s.pages.grow(&g));
    }

    /* Now we have two pages */
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);
    PageList::Node *second_page = s.pages.pages.last;

    /* Cursor should still be on the first page (where it was created) */
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);
    ASSERT_TRUE(s.cursor.page_pin->node != second_page);

    const size_t second_page_styles_cap = second_page->capacity().styles;
    const size_t cursor_page_styles_cap = s.cursor.page_pin->node->capacity().styles;

    /* Call increaseCapacity on the second page (NOT the cursor's page) */
    PageList::Node *new_second_page = nullptr;
    NOERR(s.increaseCapacity(second_page, PageList::IncreaseCapacity::styles, &new_second_page));

    /* The second page should have increased capacity */
    ASSERT_TRUE(second_page_styles_cap * 2 == new_second_page->capacity().styles);

    /* The cursor's page (first page) should be unchanged */
    ASSERT_TRUE(cursor_page_styles_cap == s.cursor.page_pin->node->capacity().styles);

    /* Cursor state should be completely unchanged since we didn't touch its page */
    ASSERT_TRUE(old_style.eql(s.cursor.style));
    ASSERT_TRUE(old_style_id == s.cursor.style_id);
    ASSERT_TRUE(old_hyperlink == s.cursor.hyperlink);
    ASSERT_TRUE(old_hyperlink_id == s.cursor.hyperlink_id);

    /* Verify hyperlink is still valid */
    ASSERT_TRUE(s.cursor.hyperlink != nullptr);
    EXPECT_STR("https://example.com/", std::string((const char *)s.cursor.hyperlink->uri, s.cursor.hyperlink->uri_len));
}

/* Wisp: while (page.styles.add(page.memory, .{ .bg_color = .{ .rgb = @bitCast(n) } })) |_| n += 1 else |_| {} */
static void fillStyles(Page *page, size_t max_items = SIZE_MAX) {
    for (size_t n = 1; n < max_items; n++) {
        style::Style st;
        st.bg_color = style::Style::Color::makeRgb(
            style::RGB((uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF), (uint8_t)((n >> 16) & 0xFF)));
        style::Id id;
        if (page->styles.add((const void *)page->memory, st, &id) != ref_counted_set::AddError::none) break;
    }
}

TEST(screen, Screen__cursorDown_to_page_with_insufficient_capacity) {
    /* Regression test for https://github.com/ghostty-org/ghostty/issues/10282
     *
     * This test exposes a use-after-realloc bug in cursorDown (and similar
     * cursor movement functions). The bug pattern:
     *
     * 1. cursorDown creates a by-value copy of the pin via page_pin.down(n)
     * 2. cursorChangePin is called, which may trigger increaseCapacity
     *    if the target page's style map is full
     * 3. increaseCapacity frees the old page and creates a new one
     * 4. The local pin copy still points to the freed page
     * 5. rowAndCell() on the stale pin accesses freed memory
     *
     * Small screen to make page boundary crossing easy to set up */
    SCREEN(s, 10, 3, (size_t)1);

    /* Scroll down enough to create a second page */
    Page *start_page = s.pages.pages.last->page();
    ASSERT_TRUE(scrollToNewPage(s, start_page));

    /* Cursor should now be on a new page */
    Page *new_page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(start_page != new_page);

    /* Fill new_page's style map to capacity. When we move INTO this page
     * with a style set, increaseCapacity will be triggered. */
    {
        new_page->pauseIntegrityChecks(true);
        fillStyles(new_page);
        new_page->pauseIntegrityChecks(false);
        new_page->assertIntegrity();
    }

    /* Move cursor to start of active area and set a style */
    s.cursorAbsolute(0, 0);
    NOERR(s.setAttribute(attr(A::bold)));
    ASSERT_TRUE(s.cursor.style.flags.bold);
    ASSERT_TRUE(s.cursor.style_id != style::default_id);

    /* Find the row just before the page boundary */
    bool found = false;
    for (size_t row = 0; row < (size_t)(s.pages.rows - 1); row++) {
        s.cursorAbsolute(0, (size::CellCountInt)row);
        PageList::Node *cur_node = s.cursor.page_pin->node;
        const Maybe<Pin> next_pin = s.cursor.page_pin->down(1);
        if (next_pin.has) {
            if (next_pin.value.node != cur_node) {
                /* Cursor is at 'row', moving down crosses to new_page */
                ASSERT_TRUE(next_pin.value.node->page() == new_page);

                /* This cursorDown triggers the bug: the local page_pin copy
                 * becomes stale after increaseCapacity, causing rowAndCell()
                 * to access freed memory. */
                s.cursorDown(1);

                /* If the fix is applied, verify correct state */
                ASSERT_TRUE(s.cursor.y == row + 1);
                ASSERT_TRUE(s.cursor.style.flags.bold);
                found = true;
                break;
            }
        }
    }
    /* Didn't find boundary */
    ASSERT_TRUE(found);
}

TEST(screen, Screen_setAttribute_increases_capacity_when_style_map_is_full) {
    /* Tests that setAttribute succeeds when the style map is full by
     * increasing page capacity. When capacity is at max and increaseCapacity
     * returns OutOfSpace, manualStyleUpdate will split the page instead.
     *
     * Use a small screen with multiple rows */
    SCREEN(s, 10, 5, (size_t)10);

    /* Write content to multiple rows */
    WRITE(s, "line1\nline2\nline3\nline4\nline5");

    /* Get the page and fill its style map to capacity */
    Page *page = s.cursor.page_pin->node->page();
    const size_t original_styles_capacity = page->capacity.styles;

    /* Fill the style map to capacity using the StyleSet's layout capacity
     * which accounts for the load factor */
    {
        page->pauseIntegrityChecks(true);
        const size_t max_items = page->styles.layout.cap;
        fillStyles(page, max_items);
        page->pauseIntegrityChecks(false);
        page->assertIntegrity();
    }

    /* Now try to set a new unique attribute that would require a new style slot
     * This should succeed by increasing capacity (or splitting if at max capacity) */
    NOERR(s.setAttribute(attr(A::bold)));

    /* The style should have been applied (bold flag set) */
    ASSERT_TRUE(s.cursor.style.flags.bold);

    /* The cursor should have a valid non-default style_id */
    ASSERT_TRUE(s.cursor.style_id != style::default_id);

    /* Either the capacity increased or the page was split/changed */
    Page *current_page = s.cursor.page_pin->node->page();
    const bool capacity_increased = current_page->capacity.styles > original_styles_capacity;
    const bool page_changed = current_page != page;
    ASSERT_TRUE(capacity_increased || page_changed);
}

TEST(screen, Screen_setAttribute_splits_page_on_OutOfSpace_at_max_styles) {
    SCREEN(s, 10, 10, (size_t)0);

    /* Write content to multiple rows so we have something to split */
    WRITE(s, "line1\nline2\nline3\nline4\nline5");

    /* Remember the original node */
    PageList::Node *original_node = s.cursor.page_pin->node;

    /* Increase the page's style capacity to max by repeatedly calling increaseCapacity
     * Use Screen.increaseCapacity to properly maintain cursor state */
    const size_t max_styles = 0xFFFF;
    while (s.cursor.page_pin->node->capacity().styles < max_styles) {
        PageList::Node *n;
        if (s.increaseCapacity(s.cursor.page_pin->node, PageList::IncreaseCapacity::styles, &n) !=
            PageList::IncreaseCapacityError::none)
            break;
    }

    /* Get the page reference after increaseCapacity - cursor may have moved */
    Page *page = s.cursor.page_pin->node->page();
    ASSERT_TRUE(max_styles == page->capacity.styles);

    /* Fill the style map to capacity using the StyleSet's layout capacity
     * which accounts for the load factor */
    {
        page->pauseIntegrityChecks(true);
        const size_t max_items = page->styles.layout.cap;
        fillStyles(page, max_items);
        page->pauseIntegrityChecks(false);
        page->assertIntegrity();
    }

    /* Track the node before setAttribute */
    PageList::Node *node_before_set = s.cursor.page_pin->node;

    /* Now try to set a new unique attribute that would require a new style slot
     * At max capacity, increaseCapacity will return OutOfSpace, triggering page split */
    NOERR(s.setAttribute(attr(A::bold)));

    /* The style should have been applied (bold flag set) */
    ASSERT_TRUE(s.cursor.style.flags.bold);

    /* The cursor should have a valid non-default style_id */
    ASSERT_TRUE(s.cursor.style_id != style::default_id);

    /* The page should have been split */
    const bool page_was_split = s.cursor.page_pin->node != node_before_set || node_before_set->next != nullptr ||
                                node_before_set->prev != nullptr || s.cursor.page_pin->node != original_node;
    ASSERT_TRUE(page_was_split);
}

TEST(screen, Screen__promptClickMove_line_right_basic) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Move cursor back to start of input (column 2, the 'h') */
    s.cursorAbsolute(2, 0);

    /* Click on first 'l' (column 4), should require 2 right movements (h->e->l) */
    const Pin click_pin = pinAt(s, Point::active(4, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(2 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_line_right_cursor_not_on_input) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");
    s.cursorSetSemanticContent(SCS::makeOutput());

    /* Move cursor back to prompt area (column 0, the '>') */
    s.cursorAbsolute(0, 0);

    /* Cursor is on prompt, not input - should return zero */
    const Pin click_pin = pinAt(s, Point::active(4, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(result.left == 0 && result.right == 0);
}

TEST(screen, Screen__promptClickMove_line_right_click_on_same_position) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Move cursor to column 4 */
    s.cursorAbsolute(4, 0);

    /* Click on same position - no movement needed */
    const Pin click_pin = pinAt(s, Point::active(4, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(result.left == 0 && result.right == 0);
}

TEST(screen, Screen__promptClickMove_line_right_skips_non_input_cells) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write: "> h" then output "X" then input "llo" */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "h");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "X");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "llo");

    /* Move cursor to column 2 (the 'h') */
    s.cursorAbsolute(2, 0);

    /* Click on 'l' at column 5 - should skip the 'X' output cell
     * Movement: h (start) -> l (col 4) -> l (col 5) = 2 right movements */
    const Pin click_pin = pinAt(s, Point::active(5, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(2 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_line_right_soft_wrapped_line) {

    SCREEN(s, 10, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input that wraps */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    /* Write 8 chars of input, first row has 2 for prompt + 8 input = 10 cols */
    WRITE(s, "abcdefgh");
    /* Continue on next row (soft-wrapped) */
    WRITE(s, "ij");

    /* Verify soft wrap occurred */
    EXPECT_DUMP(s, viewport, "> abcdefgh\nij");

    /* Move cursor to column 2 (the 'a') */
    s.cursorAbsolute(2, 0);

    /* Click on 'j' at column 1, row 1 - should count all input cells
     * Movement: a->b->c->d->e->f->g->h->i->j = 9 right movements */
    const Pin click_pin = pinAt(s, Point::active(1, 1));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(9 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_disabled_when_click_is_none) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Click mode is .none by default (disabled) */
    ASSERT_TRUE(Screen::SemanticPrompt::SemanticClick::Kind::none == s.semantic_prompt.click.tag);

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Move cursor to start of input */
    s.cursorAbsolute(2, 0);

    /* Click should return zero since click mode is disabled */
    const Pin click_pin = pinAt(s, Point::active(4, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(result.left == 0 && result.right == 0);
}

TEST(screen, Screen__promptClickMove_line_right_stops_at_hard_wrap) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write prompt and input on first line */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");
    /* Hard wrap (newline) */
    WRITE(s, "\n");
    WRITE(s, "world");

    /* Move cursor to column 2 (the 'h') */
    s.cursorAbsolute(2, 0);

    /* Click on 'w' at column 0, row 1 - but line mode stops at hard wrap
     * Should only move to end of first line: h->e->l->l->o = 4 movements */
    const Pin click_pin = pinAt(s, Point::active(0, 1));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    /* Should stop at end of first line, not cross hard wrap */
    ASSERT_TRUE(5 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_line_right_stops_at_non_continuation_row) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Row 0: PROMPT "> hello" */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello\n");

    /* Row 1: CONTINUATION "world" */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::continuation));
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "world\n");

    /* Row 2: NEW PROMPT "> again" */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "again");

    /* Verify content */
    EXPECT_DUMP(s, viewport, "> hello\nworld\n> again");

    /* Move cursor to 'w' at column 0, row 1 */
    s.cursorAbsolute(0, 1);

    /* Click on 'a' at column 2, row 2 - but row 2 is a new prompt
     * Should stop at end of "world": w->o->r->l->d = 4 movements */
    const Pin click_pin = pinAt(s, Point::active(2, 2));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    /* Should stop at 'd' (end of world), not cross to new prompt */
    ASSERT_TRUE(5 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_line_left_basic) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Cursor is at column 7 (after 'o'), move it to column 6 (the 'o') */
    s.cursorAbsolute(6, 0);

    /* Click on 'h' (column 2), should require 4 left movements (o->l->l->e->h) */
    const Pin click_pin = pinAt(s, Point::active(2, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(4 == result.left);
    ASSERT_TRUE(0 == result.right);
}

TEST(screen, Screen__promptClickMove_line_left_skips_non_input_cells) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write: "> h" then output "X" then input "llo" */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "h");
    s.cursorSetSemanticContent(SCS::makeOutput());
    WRITE(s, "X");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "llo");

    /* Move cursor to column 6 (the 'o') */
    s.cursorAbsolute(6, 0);

    /* Click on 'h' at column 2 - should skip the 'X' output cell
     * Movement: o->l->l->h = 3 left movements (skipping X) */
    const Pin click_pin = pinAt(s, Point::active(2, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(3 == result.left);
    ASSERT_TRUE(0 == result.right);
}

TEST(screen, Screen__promptClickMove_line_left_soft_wrapped_line) {

    SCREEN(s, 10, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input that wraps */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    /* Write 8 chars of input, first row has 2 for prompt + 8 input = 10 cols */
    WRITE(s, "abcdefgh");
    /* Continue on next row (soft-wrapped) */
    WRITE(s, "ij");

    /* Verify soft wrap occurred */
    EXPECT_DUMP(s, viewport, "> abcdefgh\nij");

    /* Cursor is at column 2, row 1 (after 'j'). Move to 'j' at column 1. */
    s.cursorAbsolute(1, 1);

    /* Click on 'a' at column 2, row 0 - should count all input cells backwards
     * Movement: j->i->h->g->f->e->d->c->b->a = 9 left movements */
    const Pin click_pin = pinAt(s, Point::active(2, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(9 == result.left);
    ASSERT_TRUE(0 == result.right);
}

TEST(screen, Screen__promptClickMove_line_left_stops_at_hard_wrap) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write prompt and input on first line, then hard wrap */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");
    /* Hard wrap (newline) */
    WRITE(s, "\n");
    WRITE(s, "world");

    /* Move cursor to 'd' at column 4, row 1 (an actual input cell) */
    s.cursorAbsolute(4, 1);

    /* Click on 'h' at column 2, row 0 - but line mode stops at hard wrap
     * Should only move to start of second line, not cross to row 0 */
    const Pin click_pin = pinAt(s, Point::active(2, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    /* Should stop at start of second line: d->l->r->o->w = 4 movements */
    ASSERT_TRUE(4 == result.left);
    ASSERT_TRUE(0 == result.right);
}

TEST(screen, Screen__promptClickMove_click_right_of_input_same_line) {

    /* Set up: "> hello" where "> " is prompt and "hello" is input
     * Clicking to the right of the 'o' should move cursor past the input */

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Move cursor to start of input (column 2, the 'h') */
    s.cursorAbsolute(2, 0);

    /* Click beyond the input (column 15) - should move to one past the 'o' */
    const Pin click_pin = pinAt(s, Point::active(15, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(5 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_click_right_of_input_cursor_at_end) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Cursor is already at column 7 (one past 'o') after writing
     * Click beyond the input (column 15) - no movement needed since
     * cursor is already at the end position */
    const Pin click_pin = pinAt(s, Point::active(15, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(0 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_click_right_of_input_on_lower_line) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Move cursor to start of input (column 2, the 'h') */
    s.cursorAbsolute(2, 0);

    /* Click on a lower line (row 1) - should move to end of input
     * This is outside the prompt area so should clamp to end */
    const Pin click_pin = pinAt(s, Point::active(5, 1));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    /* From 'h', we need to pass e, l, l, o (4 cells) + 1 past end = 5 */
    ASSERT_TRUE(5 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_click_right_of_input_cursor_at_end_lower_line) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Cursor is at column 7 after writing (one past 'o')
     * Click on a lower line (row 1) - cursor already at end, no movement needed */
    const Pin click_pin = pinAt(s, Point::active(5, 1));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(0 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__promptClickMove_click_right_of_input_cursor_on_last_char) {

    SCREEN(s, 20, 5, (size_t)0);

    /* Enable line click mode */
    s.semantic_prompt.click.tag = Screen::SemanticPrompt::SemanticClick::Kind::cl;
    s.semantic_prompt.click.cl = terminal::osc::semantic_prompt::Click::line;

    /* Write a prompt and input */
    s.cursorSetSemanticContent(SCS::makePrompt(PromptKind::initial));
    WRITE(s, "> ");
    s.cursorSetSemanticContent(SCS::makeInput(SCS::InputClear::clear_explicit));
    WRITE(s, "hello");

    /* Move cursor to last input char (column 6, the 'o') */
    s.cursorAbsolute(6, 0);

    /* Click beyond the input (column 15) */
    const Pin click_pin = pinAt(s, Point::active(15, 0));
    const Screen::PromptClickMove result = s.promptClickMove(click_pin);

    ASSERT_TRUE(1 == result.right);
    ASSERT_TRUE(0 == result.left);
}

TEST(screen, Screen__cursorScrollRegionUp_recycled_row_has_default_metadata) {

    SCREEN(s, 5, 5, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL\n4MNOP\n5QRST");

    /* Simulate the top region row being part of a soft-wrapped,
     * prompt-marked line. Its Row storage is recycled as the new
     * blank cursor row and must not retain the metadata. */
    {
        const PageList::Cell rac = s.pages.getCell(Point::active()).value;
        rac.row->setWrap(true);
        rac.row->setWrapContinuation(true);
        rac.row->setSemanticPrompt(Row::SemanticPrompt::prompt);
    }

    s.cursorAbsolute(1, 2);
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    {
        const PageList::Cell rac = s.pages.getCell(Point::active(0, 2)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
    EXPECT_DUMP(s, screen, "2EFGH\n3IJKL\n\n4MNOP\n5QRST");
}

TEST(screen, Screen__cursorScrollRegionUp_cross_page_recycled_row_has_default_metadata) {

    SCREEN(s, 10, 5, (size_t)10);

    /* We need to get the active area to span two pages so that the
     * scroll region does too, exercising the slow path (eraseRowBounded). */
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    s.pages.pages.first->page()->pauseIntegrityChecks(true);
    for (size_t i_ = 0, n_ = (size_t)(first_page_size - 3); i_ < n_; i_++) WRITE(s, "\n");
    s.pages.pages.first->page()->pauseIntegrityChecks(false);
    WRITE(s, "1A\n2B\n3C\n4D\n5E");

    /* The first row of the last page is the Row storage that ends up
     * recycled as the blank region-bottom row: the erased row's
     * storage stays on the first page (receiving this row's content
     * via clone) while this storage is cleared for the blank row. */
    {
        const PageList::Cell rac = s.pages.getCell(Point::active(0, 3)).value;
        rac.row->setWrap(true);
        rac.row->setWrapContinuation(true);
        rac.row->setSemanticPrompt(Row::SemanticPrompt::prompt);
    }

    /* Region rows 1-3 with the cursor on the region bottom, which is
     * on the second page while the region top is on the first page. */
    s.cursorAbsolute(0, 3);
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    {
        const PageList::Cell rac = s.pages.getCell(Point::active(0, 3)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
    EXPECT_DUMP(s, viewport, "1A\n3C\n4D\n\n5E");
}

TEST(screen, Screen__cursorScrollAbove_cross_page_recycled_row_has_default_metadata) {

    SCREEN(s, 10, 5, (size_t)10);

    /* Get the cursor page and the last page to differ so that
     * cursorScrollAbove takes the cross-page rotate path. */
    const size_t first_page_size = s.pages.pages.first->capacity().rows;
    s.pages.pages.first->page()->pauseIntegrityChecks(true);
    for (size_t i_ = 0, n_ = (size_t)(first_page_size - 3); i_ < n_; i_++) WRITE(s, "\n");
    s.pages.pages.first->page()->pauseIntegrityChecks(false);
    WRITE(s, "1A\n2B\n3C\n4D\n5E");
    s.cursorAbsolute(0, 1);
    ASSERT_TRUE(s.cursor.page_pin->node == s.pages.pages.first);
    ASSERT_TRUE(s.pages.pages.first->next != nullptr);

    /* The last row of the cursor page is the Row storage that gets
     * recycled as the new blank row below the cursor after its content
     * is moved down to the next page. */
    {
        const PageList::Cell rac = s.pages.getCell(Point::active(0, 2)).value;
        rac.row->setWrap(true);
        rac.row->setWrapContinuation(true);
        rac.row->setSemanticPrompt(Row::SemanticPrompt::prompt);
    }

    ASSERT_TRUE(s.cursorScrollAbove());

    /* One row scrolled into history, so the blank row is at active
     * y=1 (just below the cursor's original row). */
    {
        const PageList::Cell rac = s.pages.getCell(Point::active(0, 1)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
    EXPECT_DUMP(s, viewport, "2B\n\n3C\n4D\n5E");
}

TEST(screen, Screen__cursorDownScroll_no_scrollback_recycled_row_has_default_metadata) {

    SCREEN(s, 5, 3, (size_t)0);
    WRITE(s, "1ABCD\n2EFGH\n3IJKL");

    {
        const PageList::Cell rac = s.pages.getCell(Point::active()).value;
        rac.row->setWrap(true);
        rac.row->setWrapContinuation(true);
        rac.row->setSemanticPrompt(Row::SemanticPrompt::prompt);
    }

    s.cursorAbsolute(0, 2);
    ASSERT_TRUE(s.cursorDownScroll());

    {
        const PageList::Cell rac = s.pages.getCell(Point::active(0, 2)).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(screen, Screen__cursorDownScroll_single_row_no_scrollback_resets_metadata) {

    SCREEN(s, 5, 1, (size_t)0);
    WRITE(s, "1ABCD");

    {
        const PageList::Cell rac = s.pages.getCell(Point::active()).value;
        rac.row->setWrap(true);
        rac.row->setWrapContinuation(true);
        rac.row->setSemanticPrompt(Row::SemanticPrompt::prompt);
    }

    ASSERT_TRUE(s.cursorDownScroll());

    {
        const PageList::Cell rac = s.pages.getCell(Point::active()).value;
        ASSERT_TRUE(!rac.row->wrap());
        ASSERT_TRUE(!rac.row->wrap_continuation());
        ASSERT_TRUE(Row::SemanticPrompt::none == rac.row->semantic_prompt());
    }
}

TEST(screen, Screen__selectLine_does_not_join_lines_across_a_recycled_row) {

    SCREEN(s, 6, 5, (size_t)0);

    /* A soft-wrapped line across rows 0 and 1: row 0 gets wrap=true. */
    WRITE(s, "AAAAAAA");

    /* Scroll a region of rows 0-2 up by one: row 0 is discarded and
     * its Row storage recycled as the blank row 2. */
    s.cursorAbsolute(0, 2);
    ASSERT_TRUE(s.cursorScrollRegionUp(2));

    /* Write unrelated single-line words on the recycled row and below. */
    s.cursorAbsolute(0, 2);
    WRITE(s, "world");
    s.cursorAbsolute(0, 3);
    WRITE(s, "hello");

    /* Selecting the line "world" must not extend into "hello": these
     * are separate hard lines. A stale wrap flag on the recycled row
     * would join them. */
    {
        SEL(sel, s, s.selectLine(selLine(pinAt(s, Point::active(0, 2)))));
        EXPECT_STR("world", s.selectionString(Screen::SelectionString(sel, false)));
    }
}

/* @@TESTS@@ */

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(screen, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
