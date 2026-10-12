/* Transliterated from the test blocks in Ghostty
 * src/terminal/search/sliding_window.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * All 26 upstream `test "..."` blocks are ported. None are gated on a
 * build flag, so none are omitted.
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place by
 * StreamHolder as in the formatter/stream_terminal tests.
 */

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"
#include "vt_screen_test_helpers.hpp"

#include "../terminal/stream_terminal.hpp"
#include "../vt/highlight.hpp"
#include "../vt/search/sliding_window.hpp"

typedef wisp::vt::search::SlidingWindow SlidingWindow;
typedef wisp::vt::PageList::Node Node;
typedef wisp::vt::highlight::Flattened Flattened;
typedef wisp::vt::highlight::Untracked Untracked;

typedef wisp::terminal::stream_terminal::Handler SWHandler;
typedef wisp::terminal::stream::Stream<SWHandler> SWStream;

/* Wisp: var t: Terminal = try .init(io, alloc, .{...}); defer t.deinit(alloc) */
struct SWTermHolder {
    wisp::vt::Terminal t;
    bool ok;
    SWTermHolder(unsigned cols, unsigned rows) {
        wisp::vt::Terminal::Options o((wisp::vt::size::CellCountInt)cols,
                                      (wisp::vt::size::CellCountInt)rows);
        ok = wisp::vt::Terminal::init(talloc(), o, &t);
    }
    ~SWTermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: var s = t.vtStream(); defer s.deinit(); */
struct SWStreamHolder {
    SWStream s;
    explicit SWStreamHolder(const SWHandler &h) : s(h, streamOptions()) {}
    ~SWStreamHolder() { s.deinit(); }

    static SWStream::Options streamOptions() {
        SWStream::Options o;
        o.allocator = true;
        return o;
    }
};

#define SW_TERM(v, c, r)                                                                           \
    SWTermHolder v##_holder((c), (r));                                                             \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t;                                                          \
    SWStreamHolder v##_stream(SWHandler::init(&v));                                                \
    SWStream &v##_s = v##_stream.s

/* Wisp: var w: SlidingWindow = try .init(alloc, dir, needle); defer w.deinit(); */
struct WindowHolder {
    SlidingWindow w;
    bool ok;
    WindowHolder(SlidingWindow::Direction d, const uint8_t *needle, size_t len) {
        ok = SlidingWindow::init(talloc(), d, needle, len, &w);
    }
    ~WindowHolder() {
        if (ok) w.deinit();
    }
};

#define WINDOW(var, dir, needle)                                                                   \
    WindowHolder var##_holder(SlidingWindow::Direction::dir, (const uint8_t *)(needle),            \
                              strlen(needle));                                                     \
    ASSERT_TRUE(var##_holder.ok);                                                                  \
    SlidingWindow &var = var##_holder.w

/* Wisp: a needle built at runtime (the "keeps both pages" tests). */
#define WINDOW_BUF(var, dir, str)                                                                  \
    WindowHolder var##_holder(SlidingWindow::Direction::dir, (const uint8_t *)(str).data(),        \
                              (str).size());                                                      \
    ASSERT_TRUE(var##_holder.ok);                                                                  \
    SlidingWindow &var = var##_holder.w

/* Wisp: `_ = try w.append(node)` — the AppendResult is discarded. */
static bool appendNode(SlidingWindow &w, Node *node) {
    SlidingWindow::AppendResult r;
    return w.append(node, &r);
}

#define APPEND(w, node) ASSERT_TRUE(appendNode((w), (node)))

/* Wisp: `const h = w.next().?` */
#define NEXT(h, w)                                                                                 \
    Flattened h;                                                                                   \
    ASSERT_TRUE((w).next(&h))

/* Wisp: `try testing.expect(w.next() == null)` */
#define NO_NEXT(w)                                                                                 \
    do {                                                                                           \
        Flattened _h;                                                                              \
        ASSERT_FALSE((w).next(&_h));                                                                \
    } while (0)

/* Wisp: expectEqual(point.Point{ .active = .{ .x = X, .y = Y } },
 *                   pages.pointFromPin(.active, pin)) */
#define EXPECT_PT(pages, pin, X, Y)                                                                \
    ASSERT_TRUE(ptEq((pages).pointFromPin(point::Tag::active, (pin)),                              \
                     Point::active((wisp::vt::size::CellCountInt)(X), (uint32_t)(Y))))

/* Wisp: w.testChangeNeedle("boo") */
#define CHANGE_NEEDLE(w, s) (w).testChangeNeedle((const uint8_t *)(s), strlen(s))

/* ────────────────────────────────────────────────────────────────────── */

TEST(sliding_window, SlidingWindow_empty_on_init) {
    WINDOW(w, forward, "boo!");
    ASSERT_EQ(0, w.data.len());
    ASSERT_EQ(0, w.meta.len());
}

TEST(sliding_window, SlidingWindow_empty_needle_has_no_matches) {
    WINDOW(w, forward, "");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello");

    APPEND(w, s.pages.pages.first);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append) {
    WINDOW(w, forward, "boo!");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello. boo! hello. boo!");

    /* We want to test single-page cases. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);

    /* We should be able to find two matches. */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 0);
        EXPECT_PT(s.pages, sel.end, 10, 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 19, 0);
        EXPECT_PT(s.pages, sel.end, 22, 0);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_case_insensitive_ASCII) {
    WINDOW(w, forward, "Boo!");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello. boo! hello. boo!");

    /* We want to test single-page cases. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);

    /* We should be able to find two matches. */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 0);
        EXPECT_PT(s.pages, sel.end, 10, 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 19, 0);
        EXPECT_PT(s.pages, sel.end, 22, 0);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_single_char) {
    WINDOW(w, forward, "b");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello. boo! hello. boo!");

    /* We want to test single-page cases. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);

    /* We should be able to find two matches. */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 0);
        EXPECT_PT(s.pages, sel.end, 7, 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 19, 0);
        EXPECT_PT(s.pages, sel.end, 19, 0);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_no_match) {
    WINDOW(w, forward, "nope!");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello. boo! hello. boo!");

    /* We want to test single-page cases. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);

    /* No matches */
    NO_NEXT(w);
    NO_NEXT(w);

    /* Should still keep the page */
    ASSERT_EQ(1, w.meta.len());
}

/* Wisp: the shared two-page preamble. Fill up the first page; the final
 * bytes in the first page are "boo!". */
#define TWO_PAGES_BOO(s)                                                                           \
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;                           \
    for (size_t i = 0; i < first_page_rows - 1; i++) WRITE(s, "\n");                               \
    for (size_t i = 0; i < (size_t)s.pages.cols - 4; i++) WRITE(s, "x");                           \
    WRITE(s, "boo!");                                                                              \
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);                                        \
    WRITE(s, "\n");                                                                                \
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);                                        \
    WRITE(s, "hello. boo!")

TEST(sliding_window, SlidingWindow_two_pages) {
    WINDOW(w, forward, "boo!");

    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Add both pages */
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node->next);

    /* Search should find two matches */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 76, 22);
        EXPECT_PT(s.pages, sel.end, 79, 22);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 23);
        EXPECT_PT(s.pages, sel.end, 10, 23);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_two_pages_single_char) {
    WINDOW(w, forward, "b");

    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Add both pages */
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node->next);

    /* Search should find two matches */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 76, 22);
        EXPECT_PT(s.pages, sel.end, 76, 22);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 23);
        EXPECT_PT(s.pages, sel.end, 7, 23);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_two_pages_match_across_boundary) {
    WINDOW(w, forward, "hello, world");

    SCREEN(s, 80, 24, (size_t)1000);

    /* Fill up the first page. The final bytes in the first page
     * are "boo!" */
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;
    for (size_t i = 0; i < first_page_rows - 1; i++) WRITE(s, "\n");
    for (size_t i = 0; i < (size_t)s.pages.cols - 4; i++) WRITE(s, "x");
    WRITE(s, "hell");
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    WRITE(s, "o, world!");
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);

    /* Add both pages */
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node->next);

    /* Search should find a match */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 76, 22);
        EXPECT_PT(s.pages, sel.end, 7, 23);
    }
    NO_NEXT(w);
    NO_NEXT(w);

    /* We shouldn't prune because we don't have enough space */
    ASSERT_EQ(2, w.meta.len());
}

TEST(sliding_window, SlidingWindow_two_pages_no_match_across_boundary_with_newline) {
    WINDOW(w, forward, "hello, world");

    SCREEN(s, 80, 24, (size_t)1000);

    /* Fill up the first page. The final bytes in the first page
     * are "boo!" */
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;
    for (size_t i = 0; i < first_page_rows - 1; i++) WRITE(s, "\n");
    for (size_t i = 0; i < (size_t)s.pages.cols - 4; i++) WRITE(s, "x");
    WRITE(s, "hell");
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    WRITE(s, "\no, world!");
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);

    /* Add both pages */
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node->next);

    /* Search should NOT find a match */
    NO_NEXT(w);
    NO_NEXT(w);

    /* We shouldn't prune because we don't have enough space */
    ASSERT_EQ(2, w.meta.len());
}

TEST(sliding_window, SlidingWindow_two_pages_no_match_across_boundary_with_newline_reverse) {
    WINDOW(w, reverse, "hello, world");

    SCREEN(s, 80, 24, (size_t)1000);

    /* Fill up the first page. The final bytes in the first page
     * are "boo!" */
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;
    for (size_t i = 0; i < first_page_rows - 1; i++) WRITE(s, "\n");
    for (size_t i = 0; i < (size_t)s.pages.cols - 4; i++) WRITE(s, "x");
    WRITE(s, "hell");
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    WRITE(s, "\no, world!");
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);

    /* Add both pages in reverse order */
    Node *node = s.pages.pages.first;
    APPEND(w, node->next);
    APPEND(w, node);

    /* Search should NOT find a match */
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_two_pages_no_match_prunes_first_page) {
    WINDOW(w, forward, "nope!");

    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Add both pages */
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node->next);

    /* Search should find nothing */
    NO_NEXT(w);
    NO_NEXT(w);

    /* We should've pruned our page because the second page
     * has enough text to contain our needle. */
    ASSERT_EQ(1, w.meta.len());
}

TEST(sliding_window, SlidingWindow_two_pages_no_match_keeps_both_pages) {
    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Imaginary needle for search. Doesn't match! */
    const std::string needle(first_page_rows * (size_t)s.pages.cols, 'x');

    WINDOW_BUF(w, forward, needle);

    /* Add both pages */
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node->next);

    /* Search should find nothing */
    NO_NEXT(w);
    NO_NEXT(w);

    /* No pruning because both pages are needed to fit needle. */
    ASSERT_EQ(2, w.meta.len());
}

TEST(sliding_window, SlidingWindow_single_append_across_circular_buffer_boundary) {
    WINDOW(w, forward, "abc");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "XXXXXXXXXXXXXXXXXXXboo!XXXXX");

    /* We are trying to break a circular buffer boundary so the way we
     * do this is to duplicate the data then do a failing search. This
     * will cause the first page to be pruned. The next time we append we'll
     * put it in the middle of the circ buffer. We assert this so that if
     * our implementation changes our test will fail. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node);
    {
        /* No wrap around yet */
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] == 0);
    }

    /* Search non-match, prunes page */
    NO_NEXT(w);
    ASSERT_EQ(1, w.meta.len());

    /* Change the needle, just needs to be the same length (not a real API) */
    CHANGE_NEEDLE(w, "boo");

    /* Add new page, now wraps */
    APPEND(w, node);
    {
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] > 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 19, 0);
        EXPECT_PT(s.pages, sel.end, 21, 0);
    }
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_match_on_boundary) {
    WINDOW(w, forward, "abcd");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "o!XXXXXXXXXXXXXXXXXXXbo");

    /* We need to surgically modify the last row to be soft-wrapped */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    node->page()->getRow(node->rows() - 1)->setWrap(true);

    /* We are trying to break a circular buffer boundary so the way we
     * do this is to duplicate the data then do a failing search. This
     * will cause the first page to be pruned. The next time we append we'll
     * put it in the middle of the circ buffer. We assert this so that if
     * our implementation changes our test will fail. */
    APPEND(w, node);
    APPEND(w, node);
    {
        /* No wrap around yet */
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] == 0);
    }

    /* Search non-match, prunes page */
    NO_NEXT(w);
    ASSERT_EQ(1, w.meta.len());

    /* Change the needle, just needs to be the same length (not a real API) */
    CHANGE_NEEDLE(w, "boo!");

    /* Add new page, now wraps */
    APPEND(w, node);
    {
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] > 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 21, 0);
        EXPECT_PT(s.pages, sel.end, 1, 0);
    }
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_reversed) {
    WINDOW(w, reverse, "boo!");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello. boo! hello. boo!");

    /* We want to test single-page cases. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);

    /* We should be able to find two matches. */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 19, 0);
        EXPECT_PT(s.pages, sel.end, 22, 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 0);
        EXPECT_PT(s.pages, sel.end, 10, 0);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_no_match_reversed) {
    WINDOW(w, reverse, "nope!");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "hello. boo! hello. boo!");

    /* We want to test single-page cases. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);

    /* No matches */
    NO_NEXT(w);
    NO_NEXT(w);

    /* Should still keep the page */
    ASSERT_EQ(1, w.meta.len());
}

TEST(sliding_window, SlidingWindow_two_pages_reversed) {
    WINDOW(w, reverse, "boo!");

    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Add both pages in reverse order */
    Node *node = s.pages.pages.first;
    APPEND(w, node->next);
    APPEND(w, node);

    /* Search should find two matches (in reverse order) */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 7, 23);
        EXPECT_PT(s.pages, sel.end, 10, 23);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 76, 22);
        EXPECT_PT(s.pages, sel.end, 79, 22);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_two_pages_match_across_boundary_reversed) {
    WINDOW(w, reverse, "hello, world");

    SCREEN(s, 80, 24, (size_t)1000);

    /* Fill up the first page. The final bytes in the first page
     * are "hell" */
    const size_t first_page_rows = s.pages.pages.first->capacity().rows;
    for (size_t i = 0; i < first_page_rows - 1; i++) WRITE(s, "\n");
    for (size_t i = 0; i < (size_t)s.pages.cols - 4; i++) WRITE(s, "x");
    WRITE(s, "hell");
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    WRITE(s, "o, world!");
    ASSERT_TRUE(s.pages.pages.first != s.pages.pages.last);

    /* Add both pages in reverse order */
    Node *node = s.pages.pages.first;
    APPEND(w, node->next);
    APPEND(w, node);

    /* Search should find a match */
    {
        NEXT(h, w);
        ASSERT_EQ(2, h.chunks.size());
        ASSERT_TRUE(h.chunks[0].node == node);
        ASSERT_TRUE(h.chunks[0].serial == node->serial);
        ASSERT_TRUE(h.chunks[1].node == node->next);
        ASSERT_TRUE(h.chunks[1].serial == node->next->serial);

        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 76, 22);
        EXPECT_PT(s.pages, sel.end, 7, 23);
    }
    NO_NEXT(w);
    NO_NEXT(w);

    /* In reverse mode, the last appended meta (first original page) is large
     * enough to contain needle.len - 1 bytes, so pruning occurs */
    ASSERT_EQ(1, w.meta.len());
}

TEST(sliding_window, SlidingWindow_two_pages_no_match_prunes_first_page_reversed) {
    WINDOW(w, reverse, "nope!");

    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Add both pages in reverse order */
    Node *node = s.pages.pages.first;
    APPEND(w, node->next);
    APPEND(w, node);

    /* Search should find nothing */
    NO_NEXT(w);
    NO_NEXT(w);

    /* We should've pruned our page because the second page
     * has enough text to contain our needle. */
    ASSERT_EQ(1, w.meta.len());
}

TEST(sliding_window, SlidingWindow_two_pages_no_match_keeps_both_pages_reversed) {
    SCREEN(s, 80, 24, (size_t)1000);
    TWO_PAGES_BOO(s);

    /* Imaginary needle for search. Doesn't match! */
    const std::string needle(first_page_rows * (size_t)s.pages.cols, 'x');

    WINDOW_BUF(w, reverse, needle);

    /* Add both pages in reverse order */
    Node *node = s.pages.pages.first;
    APPEND(w, node->next);
    APPEND(w, node);

    /* Search should find nothing */
    NO_NEXT(w);
    NO_NEXT(w);

    /* No pruning because both pages are needed to fit needle. */
    ASSERT_EQ(2, w.meta.len());
}

TEST(sliding_window, SlidingWindow_single_append_across_circular_buffer_boundary_reversed) {
    WINDOW(w, reverse, "abc");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "XXXXXXXXXXXXXXXXXXXboo!XXXXX");

    /* We are trying to break a circular buffer boundary so the way we
     * do this is to duplicate the data then do a failing search. This
     * will cause the first page to be pruned. The next time we append we'll
     * put it in the middle of the circ buffer. We assert this so that if
     * our implementation changes our test will fail. */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    APPEND(w, node);
    APPEND(w, node);
    {
        /* No wrap around yet */
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] == 0);
    }

    /* Search non-match, prunes page */
    NO_NEXT(w);
    ASSERT_EQ(1, w.meta.len());

    /* Change the needle, just needs to be the same length (not a real API)
     * testChangeNeedle doesn't reverse, so pass reversed needle for reverse mode */
    CHANGE_NEEDLE(w, "oob");

    /* Add new page, now wraps */
    APPEND(w, node);
    {
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] > 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 19, 0);
        EXPECT_PT(s.pages, sel.end, 21, 0);
    }
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_match_on_boundary_reversed) {
    WINDOW(w, reverse, "abcd");

    SCREEN(s, 80, 24, (size_t)0);
    WRITE(s, "o!XXXXXXXXXXXXXXXXXXXbo");

    /* We need to surgically modify the last row to be soft-wrapped */
    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    Node *node = s.pages.pages.first;
    node->page()->getRow(node->rows() - 1)->setWrap(true);

    /* We are trying to break a circular buffer boundary so the way we
     * do this is to duplicate the data then do a failing search. This
     * will cause the first page to be pruned. The next time we append we'll
     * put it in the middle of the circ buffer. We assert this so that if
     * our implementation changes our test will fail. */
    APPEND(w, node);
    APPEND(w, node);
    {
        /* No wrap around yet */
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] == 0);
    }

    /* Search non-match, prunes page */
    NO_NEXT(w);
    ASSERT_EQ(1, w.meta.len());

    /* Change the needle, just needs to be the same length (not a real API)
     * testChangeNeedle doesn't reverse, so pass reversed needle for reverse mode */
    CHANGE_NEEDLE(w, "!oob");

    /* Add new page, now wraps */
    APPEND(w, node);
    {
        const SlidingWindow::DataBuf::Slices slices = w.data.getPtrSlice(0, w.data.len());
        ASSERT_TRUE(slices.len[0] > 0);
        ASSERT_TRUE(slices.len[1] > 0);
    }
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(s.pages, sel.start, 21, 0);
        EXPECT_PT(s.pages, sel.end, 1, 0);
    }
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_soft_wrapped) {
    WINDOW(w, forward, "boo!");

    SW_TERM(t, 4, 5);
    t_s.nextSlice("A\r\nxxboo!\r\nC");

    /* We want to test single-page cases. */
    Screen *screen = t.screens.active;
    ASSERT_TRUE(screen->pages.pages.first == screen->pages.pages.last);
    Node *node = screen->pages.pages.first;
    APPEND(w, node);

    /* We should be able to find two matches. */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(screen->pages, sel.start, 2, 1);
        EXPECT_PT(screen->pages, sel.end, 1, 2);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

TEST(sliding_window, SlidingWindow_single_append_reversed_soft_wrapped) {
    WINDOW(w, reverse, "boo!");

    SW_TERM(t, 4, 5);
    t_s.nextSlice("A\r\nxxboo!\r\nC");

    /* We want to test single-page cases. */
    Screen *screen = t.screens.active;
    ASSERT_TRUE(screen->pages.pages.first == screen->pages.pages.last);
    Node *node = screen->pages.pages.first;
    APPEND(w, node);

    /* We should be able to find two matches. */
    {
        NEXT(h, w);
        const Untracked sel = h.untracked();
        EXPECT_PT(screen->pages, sel.start, 2, 1);
        EXPECT_PT(screen->pages, sel.end, 1, 2);
    }
    NO_NEXT(w);
    NO_NEXT(w);
}

/* This tests a real bug that occurred where a whitespace-only page
 * that encodes to zero bytes would crash. */
TEST(sliding_window, SlidingWindow_append_whitespace_only_node) {
    WINDOW(w, forward, "x");

    SCREEN(s, 80, 24, (size_t)0);

    /* By setting the empty page to wrap we get a zero-byte page.
     * This is invasive but its otherwise hard to reproduce naturally
     * without creating a slow test. */
    Node *node = s.pages.pages.first;
    Row *last_row = node->page()->getRow(node->rows() - 1);
    last_row->setWrap(true);

    ASSERT_TRUE(s.pages.pages.first == s.pages.pages.last);
    APPEND(w, node);

    /* No matches expected */
    NO_NEXT(w);
}

TEST(sliding_window, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
