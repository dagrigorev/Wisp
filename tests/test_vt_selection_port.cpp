/* Transliterated from the test blocks in Ghostty src/terminal/Selection.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 */

#include "vt_screen_test_helpers.hpp"

TEST(selection, Selection__adjust_right) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A1234\nB5678\nC1234\nD5678");

    /* Simple movement right */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::right);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 3)));
    }

    /* Already at end of the line. */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(4, 1)), pinAt(s, Point::screen(4, 2)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::right);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(4, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 3)));
    }

    /* Already at end of the screen */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(4, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::right);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 3)));
    }
}

TEST(selection, Selection__adjust_left) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A1234\nB5678\nC1234\nD5678");

    /* Simple movement left */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::left);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 3)));
    }

    /* Already at beginning of the line. */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(0, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::left);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(4, 2)));
    }
}

TEST(selection, Selection__adjust_left_skips_blanks) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A1234\nB5678\nC12\nD56");

    /* Same line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(4, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::left);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 3)));
    }

    /* Edge */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(0, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::left);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(2, 2)));
    }
}

TEST(selection, Selection__adjust_up) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A\nB\nC\nD\nE");

    /* Not on the first line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::up);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(3, 2)));
    }

    /* On the first line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 0)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::up);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 0)));
    }
}

TEST(selection, Selection__adjust_down) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A\nB\nC\nD\nE");

    /* Not on the first line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 3)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::down);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(3, 4)));
    }

    /* On the last line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(4, 1)), pinAt(s, Point::screen(3, 4)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::down);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(4, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(9, 4)));
    }
}

TEST(selection, Selection__adjust_down_with_not_full_screen) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A\nB\nC");

    /* On the last line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(4, 1)), pinAt(s, Point::screen(3, 2)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::down);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(4, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(9, 2)));
    }
}

TEST(selection, Selection__adjust_home) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A\nB\nC");

    /* On the last line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(4, 1)), pinAt(s, Point::screen(1, 2)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::home);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(4, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 0)));
    }
}

TEST(selection, Selection__adjust_end_with_not_full_screen) {
    SCREEN(s, 10, 10, (size_t)0);
    WRITE(s, "A\nB\nC");

    /* On the last line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(4, 0)), pinAt(s, Point::screen(1, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::end);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(4, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(9, 2)));
    }
}

TEST(selection, Selection__adjust_beginning_of_line) {
    SCREEN(s, 8, 10, (size_t)0);
    WRITE(s, "A12 B34\nC12 D34");

    /* Not at beginning of the line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(5, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::beginning_of_line);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 1)));
    }

    /* Already at beginning of the line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(0, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::beginning_of_line);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(5, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 1)));
    }

    /* End pin moves to start pin */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(0, 1)), pinAt(s, Point::screen(5, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::beginning_of_line);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(0, 1)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(0, 1)));
    }
}

TEST(selection, Selection__adjust_end_of_line) {
    SCREEN(s, 8, 10, (size_t)0);
    WRITE(s, "A12 B34\nC12 D34");

    /* Not at end of the line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 0)), pinAt(s, Point::screen(1, 0)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::end_of_line);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }

    /* Already at end of the line */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 0)), pinAt(s, Point::screen(7, 0)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::end_of_line);

        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(1, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }

    /* End pin moves to start pin */
    {
        Selection sel = Selection::init(pinAt(s, Point::screen(7, 0)), pinAt(s, Point::screen(1, 0)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        sel.adjust(&s, Selection::Adjustment::end_of_line);

        /* Start line */
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.start()), Point::screen(7, 0)));
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, sel.end()), Point::screen(7, 0)));
    }
}

TEST(selection, Selection__order__standard) {

    SCREEN(s, 100, 100, (size_t)1);

    {
        /* forward, multi-line */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(2, 2)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
    {
        /* reverse, multi-line */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 2)), pinAt(s, Point::screen(2, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::reverse);
    }
    {
        /* forward, same-line */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(3, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
    {
        /* forward, single char */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(2, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
    {
        /* reverse, single line */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(1, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::reverse);
    }
}

TEST(selection, Selection__rectangle_corners_clamp_across_mixed_width_pages) {
    SCREEN(s, 4, 2, (size_t)0,);

    PageList::Node *first = s.pages.pages.first;
    ASSERT_TRUE(s.pages.split(Pin(first, 1, 0)) == PageList::SplitError::none);
    PageList::Node *second = first->next;
    second->page()->size.cols = 2;

    const Selection sel = Selection::init(Pin(first, 0, 3), Pin(second, 0, 1), true);
    ASSERT_TRUE(Selection::Order::mirrored_forward == sel.order(&s));

    const Pin bottom_right = sel.bottomRight(&s);
    (void)bottom_right.rowAndCell();
    ASSERT_TRUE(Pin(second, 0, 1).eql(bottom_right));
}

TEST(selection, Selection__order__rectangle) {

    SCREEN(s, 100, 100, (size_t)1);

    /* Conventions:
     * TL - top left
     * BL - bottom left
     * TR - top right
     * BR - bottom right */
    {
        /* forward (TL -> BR) */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(2, 2)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
    {
        /* reverse (BR -> TL) */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 2)), pinAt(s, Point::screen(1, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::reverse);
    }
    {
        /* mirrored_forward (TR -> BL) */
        Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 3)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::mirrored_forward);
    }
    {
        /* mirrored_reverse (BL -> TR) */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 3)), pinAt(s, Point::screen(3, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::mirrored_reverse);
    }
    {
        /* forward, single line (left -> right ) */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
    {
        /* reverse, single line (right -> left) */
        Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::reverse);
    }
    {
        /* forward, single column (top -> bottom) */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(2, 3)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
    {
        /* reverse, single column (bottom -> top) */
        Selection sel = Selection::init(pinAt(s, Point::screen(2, 3)), pinAt(s, Point::screen(2, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::reverse);
    }
    {
        /* forward, single cell */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(1, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;

        ASSERT_TRUE(sel.order(&s) == Selection::Order::forward);
    }
}

TEST(selection, topLeft) {

    SCREEN(s, 10, 10, (size_t)0);
    {
        /* forward */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin tl = sel.topLeft(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, tl), Point::screen(1, 1)));
    }
    {
        /* reverse */
        Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin tl = sel.topLeft(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, tl), Point::screen(1, 1)));
    }
    {
        /* mirrored_forward */
        Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 3)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin tl = sel.topLeft(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, tl), Point::screen(1, 1)));
    }
    {
        /* mirrored_reverse */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 3)), pinAt(s, Point::screen(3, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin tl = sel.topLeft(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, tl), Point::screen(1, 1)));
    }
}

TEST(selection, bottomRight) {

    SCREEN(s, 10, 10, (size_t)0);
    {
        /* forward */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin br = sel.bottomRight(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, br), Point::screen(3, 1)));
    }
    {
        /* reverse */
        Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 1)), false);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin br = sel.bottomRight(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, br), Point::screen(3, 1)));
    }
    {
        /* mirrored_forward */
        Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 3)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin br = sel.bottomRight(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, br), Point::screen(3, 3)));
    }
    {
        /* mirrored_reverse */
        Selection sel = Selection::init(pinAt(s, Point::screen(1, 3)), pinAt(s, Point::screen(3, 1)), true);
        SelDeinit sel_deinit = {&sel, &s};
        (void)sel_deinit;
        const Pin br = sel.bottomRight(&s);
        ASSERT_TRUE(ptEq(s.pages.pointFromPin(point::Tag::screen, br), Point::screen(3, 3)));
    }
}

TEST(selection, ordered) {

    SCREEN(s, 10, 10, (size_t)0);
    {
        /* forward */
        const Selection sel = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 1)), false);
        const Selection sel_reverse = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 1)), false);
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::forward).eql(sel));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::reverse).eql(sel_reverse));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::mirrored_forward).eql(sel));
    }
    {
        /* reverse */
        const Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 1)), false);
        const Selection sel_forward = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 1)), false);
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::forward).eql(sel_forward));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::reverse).eql(sel));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::mirrored_forward).eql(sel_forward));
    }
    {
        /* mirrored_forward */
        const Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(1, 3)), true);
        const Selection sel_forward = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 3)), true);
        const Selection sel_reverse = Selection::init(pinAt(s, Point::screen(3, 3)), pinAt(s, Point::screen(1, 1)), true);
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::forward).eql(sel_forward));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::reverse).eql(sel_reverse));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::mirrored_reverse).eql(sel_forward));
    }
    {
        /* mirrored_reverse */
        const Selection sel = Selection::init(pinAt(s, Point::screen(1, 3)), pinAt(s, Point::screen(3, 1)), true);
        const Selection sel_forward = Selection::init(pinAt(s, Point::screen(1, 1)), pinAt(s, Point::screen(3, 3)), true);
        const Selection sel_reverse = Selection::init(pinAt(s, Point::screen(3, 3)), pinAt(s, Point::screen(1, 1)), true);
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::forward).eql(sel_forward));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::reverse).eql(sel_reverse));
        ASSERT_TRUE(sel.ordered(&s, Selection::Order::mirrored_forward).eql(sel_forward));
    }
}

TEST(selection, Selection__contains) {

    SCREEN(s, 10, 10, (size_t)0);
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 2)), false);

        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(6, 1))));
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(1, 2))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(1, 1))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(5, 2))));
    }

    /* Reverse */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(3, 2)), pinAt(s, Point::screen(5, 1)), false);

        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(6, 1))));
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(1, 2))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(1, 1))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(5, 2))));
    }

    /* Single line */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(8, 1)), false);

        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(6, 1))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(2, 1))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(9, 1))));
    }
}

TEST(selection, Selection__contains__rectangle) {

    SCREEN(s, 15, 15, (size_t)0);
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(3, 3)), pinAt(s, Point::screen(7, 9)), true);

        /* Center */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(5, 6))));
        /* Left border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(3, 6))));
        /* Right border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(7, 6))));
        /* Top border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(5, 3))));
        /* Bottom border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(5, 9))));

        /* Above center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(5, 2))));
        /* Below center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(5, 10))));
        /* Left center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(2, 6))));
        /* Right center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(8, 6))));
        /* Just right of top right */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(8, 3))));
        /* Just left of bottom left */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(2, 9))));
    }

    /* Reverse */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(7, 9)), pinAt(s, Point::screen(3, 3)), true);

        /* Center */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(5, 6))));
        /* Left border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(3, 6))));
        /* Right border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(7, 6))));
        /* Top border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(5, 3))));
        /* Bottom border */
        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(5, 9))));

        /* Above center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(5, 2))));
        /* Below center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(5, 10))));
        /* Left center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(2, 6))));
        /* Right center */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(8, 6))));
        /* Just right of top right */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(8, 3))));
        /* Just left of bottom left */
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(2, 9))));
    }

    /* Single line
     * NOTE: This is the same as normal selection but we just do it for brevity */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(10, 1)), true);

        ASSERT_TRUE(sel.contains(&s, pinAt(s, Point::screen(6, 1))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(2, 1))));
        ASSERT_TRUE(!sel.contains(&s, pinAt(s, Point::screen(12, 1))));
    }
}

TEST(selection, Selection__containedRow) {
    SCREEN(s, 10, 5, (size_t)0);

    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(5, 1)), pinAt(s, Point::screen(3, 3)), false);

        /* Not contained */
        ASSERT_TRUE(!sel.containedRow(&s, pinAt(s, Point::screen(1, 4))).has);

        /* Start line */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(1, 1))), Selection::init(sel.start(), pinAt(s, Point::screen(s.pages.cols - 1, 1)), false)));

        /* End line */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(2, 3))), Selection::init(pinAt(s, Point::screen(0, 3)), sel.end(), false)));

        /* Middle line */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(2, 2))), Selection::init(pinAt(s, Point::screen(0, 2)), pinAt(s, Point::screen(s.pages.cols - 1, 2)), false)));
    }

    /* Rectangle */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(6, 3)), true);

        /* Not contained */
        ASSERT_TRUE(!sel.containedRow(&s, pinAt(s, Point::screen(1, 4))).has);

        /* Start line */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(1, 1))), Selection::init(pinAt(s, Point::screen(3, 1)), pinAt(s, Point::screen(6, 1)), true)));

        /* End line */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(2, 3))), Selection::init(pinAt(s, Point::screen(3, 3)), pinAt(s, Point::screen(6, 3)), true)));

        /* Middle line */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(2, 2))), Selection::init(pinAt(s, Point::screen(3, 2)), pinAt(s, Point::screen(6, 2)), true)));
    }

    /* Single-line selection */
    {
        const Selection sel = Selection::init(pinAt(s, Point::screen(2, 1)), pinAt(s, Point::screen(6, 1)), false);

        /* Not contained */
        ASSERT_TRUE(!sel.containedRow(&s, pinAt(s, Point::screen(1, 0))).has);
        ASSERT_TRUE(!sel.containedRow(&s, pinAt(s, Point::screen(1, 2))).has);

        /* Contained */
        ASSERT_TRUE(selEq(sel.containedRow(&s, pinAt(s, Point::screen(1, 1))), sel));
    }
}

TEST(selection, Selection__containedRow_clamps_mixed_width_pages) {
    SCREEN(s, 4, 3, (size_t)0,);

    PageList::Node *first = s.pages.pages.first;
    ASSERT_TRUE(s.pages.split(Pin(first, 2, 0)) == PageList::SplitError::none);
    ASSERT_TRUE(s.pages.split(Pin(first, 1, 0)) == PageList::SplitError::none);
    PageList::Node *middle = first->next;
    PageList::Node *last = middle->next;
    middle->page()->size.cols = 2;

    const Selection linear = Selection::init(Pin(first, 0, 1), Pin(last, 0, 1), false);
    const Selection linear_row = linear.containedRow(&s, Pin(middle, 0, 0)).value;
    (void)linear_row.end().rowAndCell();
    ASSERT_TRUE(Pin(middle, 0, 0).eql(linear_row.start()));
    ASSERT_TRUE(Pin(middle, 0, 1).eql(linear_row.end()));

    const Selection rectangle = Selection::init(Pin(first, 0, 1), Pin(last, 0, 3), true);
    const Selection rectangle_row = rectangle.containedRow(&s, Pin(middle, 0, 0)).value;
    (void)rectangle_row.end().rowAndCell();
    ASSERT_TRUE(Pin(middle, 0, 1).eql(rectangle_row.start()));
    ASSERT_TRUE(Pin(middle, 0, 1).eql(rectangle_row.end()));
}

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(selection, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
