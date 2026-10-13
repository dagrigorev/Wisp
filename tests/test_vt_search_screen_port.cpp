/* Transliterated from the test blocks in Ghostty
 * src/terminal/search/screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * All 24 upstream `test "..."` blocks are ported. None are gated on a
 * build flag (kitty_graphics / glyph_protocol / tmux_control_mode), so
 * none are omitted.
 *
 * Mapping:
 *   var t: Terminal = try .init(io, alloc, .{...})    SS_TERM / SS_TERM_SB
 *   var s = t.vtStream(); defer s.deinit()            SSStreamHolder
 *   var search: ScreenSearch = try .init(...)         SEARCH(var, needle, screen)
 *   try search.searchAll()                            ASSERT_TRUE(search.searchAll())
 *   _ = try search.select(.next)                      SELECT(search, next)
 *   try testing.expect(try search.select(.next))      SELECT_TRUE / SELECT_FALSE
 *   search.selectedMatch().?.untracked()              SELECTED(sel, search)
 *   try search.matches(alloc); defer alloc.free(..)   std::vector out-param
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place by
 * SSStreamHolder as in the sliding_window/pagelist tests.
 */

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"
#include "vt_screen_test_helpers.hpp"

#include "../terminal/stream_terminal.hpp"
#include "../vt/highlight.hpp"
#include "../vt/search/screen.hpp"

typedef wisp::vt::search::ScreenSearch ScreenSearch;
typedef wisp::vt::PageList::Node Node;
typedef wisp::vt::highlight::Flattened Flattened;
typedef wisp::vt::highlight::Untracked Untracked;
typedef wisp::vt::size::CellCountInt CellCountInt;
typedef wisp::vt::page::Capacity Capacity;

typedef wisp::terminal::stream_terminal::Handler SSHandler;
typedef wisp::terminal::stream::Stream<SSHandler> SSStream;

/* Wisp: var t: Terminal = try .init(io, alloc, .{...}); defer t.deinit(alloc) */
struct SSTermHolder {
    wisp::vt::Terminal t;
    bool ok;
    SSTermHolder(unsigned cols, unsigned rows, Maybe<size_t> max_scrollback_bytes) {
        wisp::vt::Terminal::Options o((CellCountInt)cols, (CellCountInt)rows);
        o.max_scrollback_bytes = max_scrollback_bytes;
        ok = wisp::vt::Terminal::init(talloc(), o, &t);
    }
    ~SSTermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: var s = t.vtStream(); defer s.deinit(); */
struct SSStreamHolder {
    SSStream s;
    explicit SSStreamHolder(const SSHandler &h) : s(h, streamOptions()) {}
    ~SSStreamHolder() { s.deinit(); }

    static SSStream::Options streamOptions() {
        SSStream::Options o;
        o.allocator = true;
        return o;
    }
};

/* Wisp: the default max_scrollback_bytes of Terminal.Options. Upstream's
 * tests that don't name it get the struct default. */
static Maybe<size_t> ssDefaultScrollbackBytes() { return Maybe<size_t>((size_t)10000); }

#define SS_TERM(v, c, r)                                                                           \
    SSTermHolder v##_holder((c), (r), ssDefaultScrollbackBytes());                                 \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t;                                                          \
    SSStreamHolder v##_stream(SSHandler::init(&v));                                                \
    SSStream &v##_s = v##_stream.s

#define SS_TERM_SB(v, c, r, sb)                                                                    \
    SSTermHolder v##_holder((c), (r), Maybe<size_t>((size_t)(sb)));                                \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t;                                                          \
    SSStreamHolder v##_stream(SSHandler::init(&v));                                                \
    SSStream &v##_s = v##_stream.s

/* Wisp: var search: ScreenSearch = try .init(alloc, screen, needle);
 *       defer search.deinit(); */
struct SSSearchHolder {
    ScreenSearch search;
    bool ok;
    SSSearchHolder(const char *needle, Screen *screen) {
        ok = ScreenSearch::init(talloc(), screen, (const uint8_t *)needle, strlen(needle), &search);
    }
    ~SSSearchHolder() {
        if (ok) search.deinit();
    }
};

#define SEARCH(var, needle, screen)                                                                \
    SSSearchHolder var##_holder((needle), (screen));                                               \
    ASSERT_TRUE(var##_holder.ok);                                                                  \
    ScreenSearch &var = var##_holder.search

/* Wisp: `_ = try search.select(.next)` — the bool return is the OutOfMemory
 * channel and upstream discards the out-param. */
#define SELECT(search, which)                                                                      \
    do {                                                                                           \
        bool _sel = false;                                                                         \
        ASSERT_TRUE((search).select(ScreenSearch::Select::which, &_sel));                          \
    } while (0)

/* Wisp: `try testing.expect(try search.select(.next))` */
#define SELECT_TRUE(search, which)                                                                 \
    do {                                                                                           \
        bool _sel = false;                                                                         \
        ASSERT_TRUE((search).select(ScreenSearch::Select::which, &_sel));                          \
        ASSERT_TRUE(_sel);                                                                         \
    } while (0)

/* Wisp: `try testing.expect(!try search.select(.next))` */
#define SELECT_FALSE(search, which)                                                                \
    do {                                                                                           \
        bool _sel = true;                                                                          \
        ASSERT_TRUE((search).select(ScreenSearch::Select::which, &_sel));                          \
        ASSERT_FALSE(_sel);                                                                        \
    } while (0)

/* Wisp: `const sel = search.selectedMatch().?.untracked();` — the Flattened
 * is bound to a named local so the Untracked does not reference a
 * destroyed temporary. */
#define SELECTED(sel, search)                                                                      \
    Flattened sel##_hl;                                                                            \
    ASSERT_TRUE((search).selectedMatch(&sel##_hl));                                                \
    const Untracked sel = sel##_hl.untracked()

/* Wisp: `try testing.expect(search.selectedMatch() == null)` */
#define NO_SELECTED(search)                                                                        \
    do {                                                                                           \
        Flattened _hl;                                                                             \
        ASSERT_FALSE((search).selectedMatch(&_hl));                                                \
    } while (0)

/* Wisp: `try testing.expect(search.selectedMatch() != null)` */
#define HAS_SELECTED(search)                                                                       \
    do {                                                                                           \
        Flattened _hl;                                                                             \
        ASSERT_TRUE((search).selectedMatch(&_hl));                                                 \
    } while (0)

/* Wisp: `const matches = try search.matches(alloc); defer alloc.free(..)` */
#define MATCHES(var, search)                                                                       \
    std::vector<Flattened> var;                                                                    \
    ASSERT_TRUE((search).matches(&var))

/* ────────────────────────────────────────────────────────────────────── */

TEST(search_screen, simple_search) {
    SS_TERM(t, 10, 2);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)2, search.active_results.size());
    /* We don't test history results since there is overlap */

    /* Get all matches */
    MATCHES(matches, search);
    ASSERT_EQ((size_t)2, matches.size());

    {
        const Untracked sel = matches[0].untracked();
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 2)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 2)));
    }
    {
        const Untracked sel = matches[1].untracked();
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }
}

TEST(search_screen, simple_search_with_history) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    t_s.nextSlice("Fizz\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("hello.");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)0, search.active_results.size());

    /* Get all matches */
    MATCHES(matches, search);
    ASSERT_EQ((size_t)1, matches.size());

    {
        const Untracked sel = matches[0].untracked();
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }
}

TEST(search_screen, reload_active_with_history_change) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    t_s.nextSlice("Fizz\r\n");

    /* Start up our search which will populate our initial active area. */
    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    {
        MATCHES(matches, search);
        ASSERT_EQ((size_t)1, matches.size());
    }

    /* Grow into two pages so our history pin will move. */
    while (list->totalPages() < 2) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("2Fizz");

    /* Active area changed so reload */
    ASSERT_TRUE(search.reloadActive());
    ASSERT_TRUE(search.searchAll());

    /* Get all matches */
    {
        MATCHES(matches, search);
        ASSERT_EQ((size_t)2, matches.size());
        {
            const Untracked sel = matches[1].untracked();
            ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                             Point::screen(0, 0)));
            ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                             Point::screen(3, 0)));
        }
        {
            const Untracked sel = matches[0].untracked();
            ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.start),
                             Point::active(1, 1)));
            ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.end),
                             Point::active(4, 1)));
        }
    }

    /* Reset the screen which will make our pin garbage. */
    t.fullReset();
    t_s.nextSlice("WeFizzing");
    ASSERT_TRUE(search.reloadActive());
    ASSERT_TRUE(search.searchAll());

    {
        MATCHES(matches, search);
        ASSERT_EQ((size_t)1, matches.size());
        {
            const Untracked sel = matches[0].untracked();
            ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.start),
                             Point::active(2, 0)));
            ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.end),
                             Point::active(5, 0)));
        }
    }
}

TEST(search_screen, active_change_contents) {
    SS_TERM(t, 10, 5);
    t_s.nextSlice("Fuzz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.active_results.size());

    /* Erase the screen, move our cursor to the top, and change contents. */
    t_s.nextSlice("\x1b[2J\x1b[H"); /* Clear screen and move home */
    t_s.nextSlice("Bang\r\nFizz\r\nHello!");

    ASSERT_TRUE(search.reloadActive());
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.active_results.size());

    /* Get all matches */
    MATCHES(matches, search);
    ASSERT_EQ((size_t)1, matches.size());

    {
        const Untracked sel = matches[0].untracked();
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 1)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 1)));
    }
}

TEST(search_screen, select_next) {
    SS_TERM(t, 10, 2);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", t.screens.active);

    /* Initially no selection */
    NO_SELECTED(search);

    /* Select our next match (first) */
    ASSERT_TRUE(search.searchAll());
    SELECT(search, next);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 2)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 2)));
    }

    /* Next match */
    SELECT(search, next);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }

    /* Next match (wrap) */
    SELECT(search, next);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 2)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 2)));
    }
}

TEST(search_screen, select_after_resize_resets_stale_flattened_results) {
    SS_TERM_SB(t, 10, 3, SIZE_MAX);
    t_s.nextSlice("Fizz\r\nBuzz");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    SELECT_TRUE(search, next);
    HAS_SELECTED(search);
    ASSERT_EQ((CellCountInt)10, search.cols);

    /* Column reflow replaces the nodes referenced by the cached flattened
     * active result. select() calls reloadActive(), so dimension invalidation
     * must run before either path dereferences those old nodes. */
    {
        PageList::Resize r;
        r.cols = Maybe<CellCountInt>((CellCountInt)5);
        ASSERT_TRUE(t.screens.active->pages.resize(r));
    }
    ASSERT_EQ((CellCountInt)10, search.cols);

    SELECT_TRUE(search, next);
    ASSERT_EQ((CellCountInt)5, search.cols);

    SELECTED(selected, search);
    ASSERT_EQ((CellCountInt)5, selected.start.node->cols());
    ASSERT_EQ((CellCountInt)5, selected.end.node->cols());
}

TEST(search_screen, select_in_active_changes_contents_completely) {
    SS_TERM(t, 10, 5);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    SELECT(search, next);
    SELECT(search, next);
    {
        /* Initial selection is the first fizz */
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }

    /* Erase the screen, move our cursor to the top, and change contents. */
    t_s.nextSlice("\x1b[2J\x1b[H"); /* Clear screen and move home */
    t_s.nextSlice("Fuzz\r\nFizz\r\nHello!");

    ASSERT_TRUE(search.reloadActive());
    {
        /* Our selection should move to the first */
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 1)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 1)));
    }

    /* Erase the screen, redraw with same contents. */
    t_s.nextSlice("\x1b[2J\x1b[H"); /* Clear screen and move home */
    t_s.nextSlice("Fuzz\r\nFizz\r\nFizz");

    ASSERT_TRUE(search.reloadActive());
    {
        /* Our selection should not move to the first */
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 1)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 1)));
    }
}

TEST(search_screen, select_into_history) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    t_s.nextSlice("Fizz\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("hello.");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());

    /* Get all matches */
    SELECT(search, next);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }

    /* Erase the screen, redraw with same contents. */
    t_s.nextSlice("\x1b[2J\x1b[H"); /* Clear screen and move home */
    t_s.nextSlice("yo yo");

    ASSERT_TRUE(search.reloadActive());
    {
        /* Our selection should not move since the history is still active. */
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }

    /* Create some new history by adding more lines. */
    t_s.nextSlice("\r\nfizz\r\nfizz\r\nfizz");
    ASSERT_TRUE(search.reloadActive());
    {
        /* Our selection should not move since the history is still not
         * pruned. */
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }
}

TEST(search_screen, select_prev) {
    SS_TERM(t, 10, 2);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", t.screens.active);

    /* Initially no selection */
    NO_SELECTED(search);

    /* Select prev (oldest first) */
    ASSERT_TRUE(search.searchAll());
    SELECT(search, prev);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }

    /* Prev match (towards newest) */
    SELECT(search, prev);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 2)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 2)));
    }

    /* Prev match (wrap) */
    SELECT(search, prev);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }
}

TEST(search_screen, select_prev_then_next) {
    SS_TERM(t, 10, 2);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());

    /* Select next (newest first) */
    SELECT(search, next);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 2)));
    }

    /* Select next (older) */
    SELECT(search, next);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
    }

    /* Select prev (back to newer) */
    SELECT(search, prev);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 2)));
    }
}

TEST(search_screen, select_prev_with_history) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    t_s.nextSlice("Fizz\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("Fizz.");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());

    /* Select prev (oldest first, should be in history) */
    SELECT(search, prev);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.start),
                         Point::screen(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::screen, sel.end),
                         Point::screen(3, 0)));
    }

    /* Select prev (towards newer, should move to active area) */
    SELECT(search, prev);
    {
        SELECTED(sel, search);
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.start),
                         Point::active(0, 1)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.end),
                         Point::active(3, 1)));
    }
}

TEST(search_screen, select_prev_wraps_when_all_matches_are_in_history) {
    /* Regression test: when every match is in scrollback (the active area
     * has none, so active_len == 0), selecting prev from index 0 must wrap
     * to the last result without underflowing `active_len - 1`. */
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    /* Put the only match in scrollback, then scroll the active area to all
     * blank lines so it contains no match (active_len == 0, history_len == 1). */
    t_s.nextSlice("Fizz\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)0, search.active_results.size());

    /* Select the first match (idx 0), then wrap backwards. This must not
     * panic and must keep a valid selection. */
    SELECT(search, next);
    SELECT(search, prev);
    HAS_SELECTED(search);
}

TEST(search_screen, select_after_all_matches_disappear_drops_the_selection) {
    /* The wrap arithmetic in selectPrev (active_len + history_len - 1) would
     * underflow if a selection were ever live while both result lists are
     * empty. This guards the invariant that makes that unreachable: when a
     * reload/prune empties the results, the selection is dropped, so the next
     * select() hits the "no matches" guard instead of the wrap arithmetic. */
    SS_TERM(t, 10, 2);
    t_s.nextSlice("Fizz");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.active_results.size());

    /* Take a selection, then overwrite the only match so a reload finds none
     * (active and history both empty). */
    SELECT(search, next);
    HAS_SELECTED(search);
    t_s.nextSlice("\x1b[1;1H    ");

    /* Must not underflow; the selection is dropped and nothing is selected. */
    SELECT(search, prev);
    NO_SELECTED(search);
    ASSERT_EQ((size_t)0, search.active_results.size());
    ASSERT_EQ((size_t)0, search.history_results.size());
}

TEST(search_screen, select_after_partial_history_erase_drops_a_pruned_selection) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    t_s.nextSlice("error\r\n");
    Node *const first = list->pages.first;
    ASSERT_NOT_NULL(first);
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    const CellCountInt first_rows = first->rows();

    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)0, search.active_results.size());
    ASSERT_EQ((size_t)1, search.history_results.size());
    SELECT_TRUE(search, next);

    list->eraseHistory(Maybe<Point>(Point::history(0, (uint32_t)(first_rows - 1))));
    ASSERT_FALSE(search.selected.value.highlight.start->garbage);

    SELECT_FALSE(search, next);
    ASSERT_FALSE(search.selected.has);
}

TEST(search_screen, select_after_history_compaction_ignores_replaced_results) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    t_s.nextSlice("error\r\n");
    Node *const first = list->pages.first;
    ASSERT_NOT_NULL(first);
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    ASSERT_TRUE(PageList::SplitError::none ==
                list->split(Pin(first, (CellCountInt)(first->rows() / 2), 0)));

    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.history_results.size());

    Node *replacement = nullptr;
    ASSERT_TRUE(list->compact(first, &replacement));
    ASSERT_NOT_NULL(replacement);
    ASSERT_TRUE(replacement != first);

    SELECT_FALSE(search, next);
    ASSERT_FALSE(search.selected.has);
}

TEST(search_screen, select_after_partial_history_page_erase_ignores_shifted_results) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    Node *const first = list->pages.first;
    ASSERT_NOT_NULL(first);
    while (first->rows() < first->capacity().rows) t_s.nextSlice("\r\n");
    t_s.nextSlice("error");
    {
        const size_t loop_rows = (size_t)list->rows + 1;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    ASSERT_TRUE(first != list->pages.last);

    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)0, search.active_results.size());
    ASSERT_EQ((size_t)1, search.history_results.size());

    const CellCountInt old_rows = first->rows();
    list->eraseHistory(Maybe<Point>(Point::history(0, 0)));
    ASSERT_EQ((CellCountInt)(old_rows - 1), first->rows());

    SELECT_FALSE(search, next);
    ASSERT_FALSE(search.selected.has);
}

TEST(search_screen, reload_defers_pruning_unselected_history_results) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    Node *const first = list->pages.first;
    ASSERT_NOT_NULL(first);
    while (first->rows() < first->capacity().rows) t_s.nextSlice("\r\n");
    t_s.nextSlice("error");
    {
        const size_t loop_rows = (size_t)list->rows + 1;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }

    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.history_results.size());

    list->eraseHistory(Maybe<Point>(Point::history(0, 0)));

    /* Routine active refreshes don't inspect unselected history results. */
    ASSERT_TRUE(search.reloadActive());
    ASSERT_EQ((size_t)1, search.history_results.size());

    /* Selection prunes every candidate before attempting to track it. */
    SELECT_FALSE(search, next);
    ASSERT_EQ((size_t)0, search.history_results.size());
}

TEST(search_screen, reload_after_partial_history_page_erase_drops_shifted_selection_first) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    Node *const first = list->pages.first;
    ASSERT_NOT_NULL(first);
    while (first->rows() < first->capacity().rows) t_s.nextSlice("\r\n");
    t_s.nextSlice("error");
    {
        const size_t loop_rows = (size_t)list->rows + 1;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }

    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    SELECT_TRUE(search, next);
    ASSERT_TRUE(search.selected.has);

    list->eraseHistory(Maybe<Point>(Point::history(0, 0)));

    /* Advance the active boundary into another node and add matches so
     * reloadActive updates the history result offsets. It must discard the
     * stale selected result before comparing any of its captured coordinates. */
    Node *const active_node = list->getTopLeft(point::Tag::active).node;
    while (list->getTopLeft(point::Tag::active).node == active_node) {
        t_s.nextSlice("error\r\n");
    }

    ASSERT_TRUE(search.reloadActive());
    ASSERT_FALSE(search.selected.has);
}

TEST(search_screen, select_after_history_page_split_ignores_moved_results) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    Node *const first = list->pages.first;
    ASSERT_NOT_NULL(first);
    while (first->rows() < first->capacity().rows) t_s.nextSlice("\r\n");
    t_s.nextSlice("error");
    {
        const size_t loop_rows = (size_t)list->rows + 1;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }

    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.history_results.size());

    ASSERT_TRUE(PageList::SplitError::none ==
                list->split(Pin(first, (CellCountInt)(first->rows() / 2), 0)));

    SELECT_FALSE(search, next);
    ASSERT_FALSE(search.selected.has);
}

TEST(search_screen, screen_search_no_scrollback_has_no_history) {
    SS_TERM_SB(t, 10, 2, 0);

    /* Alt screen has no scrollback */
    {
        bool ok = false;
        (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
        ASSERT_TRUE(ok);
    }

    /* This will probably stop working at some point and we'll have
     * no way to test it using public APIs, but at the time of writing
     * this test, CSI 22 J (scroll complete) pushes into scrollback
     * with alt screen. */
    t_s.nextSlice("Fizz\r\n");
    t_s.nextSlice("\x1b[22J");
    t_s.nextSlice("hello.");

    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)0, search.active_results.size());

    /* Get all matches */
    MATCHES(matches, search);
    ASSERT_EQ((size_t)0, matches.size());
}

/* Wisp: defer tw.end(.reset) catch unreachable; */
struct TwEndGuard {
    ~TwEndGuard() {
        const bool ok = ScreenSearch::reloadActive_tw::end(wisp::tripwire::ResetMode::reset);
        (void)ok;
        assert(ok);
    }
};

TEST(search_screen, reloadActive_partial_history_cleanup_on_appendSlice_error) {
    /* This test verifies that when reloadActive fails at appendSlice (after
     * the loop), all FlattenedHighlight items are properly cleaned up. */
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    /* Write multiple "Fizz" matches that will end up in history.
     * We need enough content to push "Fizz" entries into scrollback. */
    t_s.nextSlice("Fizz\r\nFizz\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("Fizz.");

    /* Complete initial search */
    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());

    /* Now trigger reloadActive by adding more content that changes the
     * active/history boundary. First add more "Fizz" entries to history. */
    t_s.nextSlice("\r\nFizz\r\nFizz\r\n");
    while (list->totalPages() < 4) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }

    /* Arm the tripwire to fail at appendSlice (after the loop completes).
     * At this point, there are FlattenedHighlight items in the results list
     * that need cleanup. */
    typedef ScreenSearch::reloadActive_tw tw;
    TwEndGuard tw_guard;
    (void)tw_guard;
    tw::errorAlways(ScreenSearch::ReloadActiveTw::history_append_existing, AllocTw::OutOfMemory);

    /* reloadActive is called by select(), which should trigger the error path.
     * If the bug exists, testing.allocator will report a memory leak
     * because FlattenedHighlight items weren't cleaned up. */
    {
        bool discard = false;
        ASSERT_FALSE(search.select(ScreenSearch::Select::next, &discard));
    }
}

TEST(search_screen, reloadActive_partial_history_cleanup_on_loop_append_error) {
    /* This test verifies that when reloadActive fails inside the loop
     * (after some items have been appended), all FlattenedHighlight items
     * are properly cleaned up. */
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    /* Write multiple "Fizz" matches that will end up in history.
     * We need enough content to push "Fizz" entries into scrollback. */
    t_s.nextSlice("Fizz\r\nFizz\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("Fizz.");

    /* Complete initial search */
    SEARCH(search, "Fizz", t.screens.active);
    ASSERT_TRUE(search.searchAll());

    /* Now trigger reloadActive by adding more content that changes the
     * active/history boundary. First add more "Fizz" entries to history. */
    t_s.nextSlice("\r\nFizz\r\nFizz\r\n");
    while (list->totalPages() < 4) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }

    /* Arm the tripwire to fail after the first loop append succeeds.
     * This leaves at least one FlattenedHighlight in the results list
     * that needs cleanup. */
    typedef ScreenSearch::reloadActive_tw tw;
    TwEndGuard tw_guard;
    (void)tw_guard;
    tw::errorAfter(ScreenSearch::ReloadActiveTw::history_append_new, AllocTw::OutOfMemory, 1);

    /* reloadActive is called by select(), which should trigger the error path.
     * If the bug exists, testing.allocator will report a memory leak
     * because FlattenedHighlight items weren't cleaned up. */
    {
        bool discard = false;
        ASSERT_FALSE(search.select(ScreenSearch::Select::next, &discard));
    }
}

TEST(search_screen, select_after_clearing_scrollback) {
    /* Regression test for: https://github.com/ghostty-org/ghostty/issues/11957
     * After clearing scrollback (CSI 3J), selecting next/prev should not crash. */
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    /* Write enough content to push matches into scrollback history. */
    t_s.nextSlice("error\r\n");
    while (list->totalPages() < 3) t_s.nextSlice("error\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("error.");

    /* Start search and find all matches. */
    SEARCH(search, "error", t.screens.active);
    ASSERT_TRUE(search.searchAll());

    /* Should have matches in both history and active areas. */
    ASSERT_TRUE(search.history_results.size() > 0);
    ASSERT_TRUE(search.active_results.size() > 0);

    /* Select a match first (so we have a selection). */
    SELECT(search, next);
    ASSERT_TRUE(search.selected.has);

    /* Clear scrollback (equivalent to CSI 3J / Cmd+K erasing scrollback). */
    t.eraseDisplay(terminal::csi::EraseDisplay::scrollback, false);

    /* Selecting next/prev after clearing scrollback should not crash.
     * Before the fix, this would hit an assertion in trackPin because
     * the FlattenedHighlight contained dangling node pointers. */
    SELECT(search, next);
    SELECT(search, prev);
}

TEST(search_screen, feed_after_complete_discovers_prepended_history_pages) {
    SS_TERM_SB(t, 10, 2, SIZE_MAX);
    PageList *const list = &t.screens.active->pages;

    /* The screen begins with the tail of the needle so a restored page which
     * soft-wraps into it can complete a match across the restore boundary. */
    t_s.nextSlice("st\r\nTest\r\n");
    while (list->totalPages() < 2) t_s.nextSlice("\r\n");
    {
        const size_t loop_rows = list->rows;
        for (size_t i = 0; i < loop_rows; i++) t_s.nextSlice("\r\n");
    }
    t_s.nextSlice("Test");
    Node *const old_first = list->pages.first;
    ASSERT_NOT_NULL(old_first);

    /* Search to completion, then select the oldest match so we can verify
     * that restored (older) results never move an existing selection. */
    SEARCH(search, "Test", t.screens.active);
    ASSERT_TRUE(search.searchAll());
    ASSERT_TRUE(ScreenSearch::isComplete(search.state));
    ASSERT_EQ((size_t)1, search.active_results.size());
    ASSERT_EQ((size_t)1, search.history_results.size());
    SELECT_TRUE(search, prev);
    const size_t selected_idx = search.selected.value.idx;
    ASSERT_EQ((size_t)1, selected_idx);
    Flattened selected_before_hl;
    ASSERT_TRUE(search.selectedMatch(&selected_before_hl));
    const Untracked selected_before = selected_before_hl.untracked();
    ASSERT_TRUE(old_first == selected_before.start.node);

    /* Restore the newest history page: a full match on its first row and a
     * soft-wrapped last row ending in "Te" which joins the screen's "st". */
    {
        PageList::PageAllocation allocation;
        ASSERT_TRUE(list->allocatePage(Capacity(10, 2), &allocation));
        /* Wisp: defer allocation.deinit(); */
        struct AllocGuard {
            PageList::PageAllocation *a;
            ~AllocGuard() { a->deinit(); }
        } guard = {&allocation};
        (void)guard;
        Page *const page = allocation.page();
        page->size.rows = 2;
        {
            const char *s0 = "Test";
            for (size_t x = 0; x < 4; x++) {
                *page->getRowAndCell(x, 0).cell = Cell::init((uint32_t)(uint8_t)s0[x]);
            }
        }
        {
            const char *s1 = "xxxxxxxxTe";
            for (size_t x = 0; x < 10; x++) {
                *page->getRowAndCell(x, 1).cell = Cell::init((uint32_t)(uint8_t)s1[x]);
            }
        }
        /* Wisp: Row's flags are packed, so the field assignments are the
         * ported setters. */
        page->getRow(1)->setWrap(true);
        old_first->page()->getRow(0)->setWrapContinuation(true);
        ASSERT_TRUE(PageList::PageAllocation::FinalizeError::none ==
                    allocation.finalize(PageList::PageAllocation::Location::prepend));
    }
    Node *const newest = list->pages.first;
    ASSERT_NOT_NULL(newest);

    /* The completed search is fed like any periodic refresh. It must pick
     * the page up and go back to searching rather than staying complete. */
    ASSERT_TRUE(search.feed());
    ASSERT_FALSE(ScreenSearch::isComplete(search.state));
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)1, search.active_results.size());
    ASSERT_EQ((size_t)3, search.history_results.size());

    /* New results are older than everything cached, so they append in
     * newest-to-oldest order: the boundary match first, then the row above. */
    {
        const Untracked boundary = search.history_results[1].untracked();
        ASSERT_TRUE(newest == boundary.start.node);
        ASSERT_TRUE(old_first == boundary.end.node);
        ASSERT_EQ((CellCountInt)1, boundary.start.y);
        ASSERT_EQ((CellCountInt)8, boundary.start.x);
        ASSERT_EQ((CellCountInt)0, boundary.end.y);
        ASSERT_EQ((CellCountInt)1, boundary.end.x);
        EXPECT_STR("Test", t.screens.active->selectionString(Screen::SelectionString(
                               Selection::init(boundary.start, boundary.end, false))));
    }
    {
        const Untracked oldest = search.history_results[2].untracked();
        ASSERT_TRUE(newest == oldest.start.node);
        ASSERT_EQ((CellCountInt)0, oldest.start.y);
        ASSERT_EQ((CellCountInt)0, oldest.start.x);
    }

    /* The selection kept both its index and its target. */
    ASSERT_EQ(selected_idx, search.selected.value.idx);
    {
        Flattened selected_after_hl;
        ASSERT_TRUE(search.selectedMatch(&selected_after_hl));
        const Untracked selected_after = selected_after_hl.untracked();
        ASSERT_TRUE(selected_before.start.eql(selected_after.start));
        ASSERT_TRUE(selected_before.end.eql(selected_after.end));
    }

    /* Restore an even older page. Each incremental step is discovered from
     * the frontier left behind by the previous one. */
    {
        PageList::PageAllocation allocation;
        ASSERT_TRUE(list->allocatePage(Capacity(10, 2), &allocation));
        /* Wisp: defer allocation.deinit(); */
        struct AllocGuard {
            PageList::PageAllocation *a;
            ~AllocGuard() { a->deinit(); }
        } guard = {&allocation};
        (void)guard;
        Page *const page = allocation.page();
        page->size.rows = 2;
        {
            const char *s0 = "Test";
            for (size_t x = 0; x < 4; x++) {
                *page->getRowAndCell(x, 0).cell = Cell::init((uint32_t)(uint8_t)s0[x]);
            }
        }
        ASSERT_TRUE(PageList::PageAllocation::FinalizeError::none ==
                    allocation.finalize(PageList::PageAllocation::Location::prepend));
    }
    Node *const oldest_node = list->pages.first;
    ASSERT_NOT_NULL(oldest_node);
    ASSERT_TRUE(search.feed());
    ASSERT_TRUE(search.searchAll());
    ASSERT_EQ((size_t)4, search.history_results.size());
    ASSERT_TRUE(oldest_node == search.history_results[3].untracked().start.node);
    ASSERT_EQ(selected_idx, search.selected.value.idx);
    {
        Flattened selected_after_hl;
        ASSERT_TRUE(search.selectedMatch(&selected_after_hl));
        const Untracked selected_after = selected_after_hl.untracked();
        ASSERT_TRUE(selected_before.start.eql(selected_after.start));
        ASSERT_TRUE(selected_before.end.eql(selected_after.end));
    }

    /* With nothing new, a feed on the complete search stays complete. */
    ASSERT_TRUE(search.feed());
    ASSERT_TRUE(ScreenSearch::isComplete(search.state));
    ASSERT_EQ((size_t)5, search.matchesLen());
}

TEST(search_screen, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
