/* Transliterated from the test blocks in Ghostty
 * src/terminal/search/terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Upstream has 7 `test "..."` blocks. 6 are ported. The one omitted is:
 *
 *   "feed after complete discovers prepended snapshot history"
 *
 * It is omitted because it imports `../snapshot/main.zig` (for
 * `snapshot.encode` and `snapshot.Decoder`) and `snapshot/` is not ported
 * in Wisp. This is NOT a build-flag omission (kitty_graphics /
 * glyph_protocol / tmux_control_mode are all unrelated here): the
 * dependency simply does not exist in the port yet. TerminalSearch itself
 * never touches snapshot, so the implementation is ported in full and no
 * other test is affected.
 *
 * Mapping:
 *   var t: Terminal = try .init(io, alloc, .{...})    TS_TERM / TS_TERM_SB
 *   var stream = t.vtStream(); defer stream.deinit()  TS_STREAM
 *   var search: TerminalSearch = try .init(...)       TSEARCH(var, needle, t)
 *   the repeated `while (search.status() != .complete)` pump loop   tsPump
 *   try search.select(&t, .next, .if_needed)          SELECT_TRUE / SELECT_FALSE
 *   (try search.viewportMatches()).len                EXPECT_VPM_LEN
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place as in the
 * sibling search tests.
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"
#include "vt_screen_test_helpers.hpp"

#include "../terminal/stream_terminal.hpp"
#include "../vt/highlight.hpp"
#include "../vt/search/terminal.hpp"
/* Wisp: the search.zig index, included for compile coverage — it is only
 * aliases, so it has no tests of its own. Upstream's `test` block in that
 * file is refAllDecls over the same declarations these tests exercise. */
#include "../vt/search.hpp"

typedef wisp::vt::search::TerminalSearch TerminalSearch;
typedef wisp::vt::search::ScreenSearch ScreenSearch;
typedef wisp::vt::highlight::Flattened Flattened;
typedef wisp::vt::size::CellCountInt CellCountInt;
typedef wisp::vt::ScreenSet ScreenSet;

typedef wisp::terminal::stream_terminal::Handler TSHandler;
typedef wisp::terminal::stream::Stream<TSHandler> TSStream;

static TSStream::Options tsStreamOptions() {
    TSStream::Options o;
    o.allocator = true;
    return o;
}

/* Wisp: var t: Terminal = try .init(io, alloc, .{...}); defer t.deinit(alloc) */
struct TSTermHolder {
    wisp::vt::Terminal t;
    bool ok;
    TSTermHolder(unsigned cols, unsigned rows, bool unlimited_scrollback) {
        wisp::vt::Terminal::Options o((CellCountInt)cols, (CellCountInt)rows);
        if (unlimited_scrollback) {
            /* Wisp: .max_scrollback_bytes = std.math.maxInt(usize) */
            o.max_scrollback_bytes = Maybe<size_t>(SIZE_MAX);
        }
        ok = wisp::vt::Terminal::init(talloc(), o, &t);
    }
    ~TSTermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: var stream = t.vtStream(); defer stream.deinit(); */
struct TSStreamHolder {
    TSStream s;
    explicit TSStreamHolder(const TSHandler &h) : s(h, tsStreamOptions()) {}
    ~TSStreamHolder() { s.deinit(); }
};

#define TS_TERM(v, c, r)                                                                           \
    TSTermHolder v##_holder((c), (r), false);                                                      \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t

#define TS_TERM_SB(v, c, r)                                                                        \
    TSTermHolder v##_holder((c), (r), true);                                                       \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t

#define TS_STREAM(v)                                                                               \
    TSStreamHolder v##_stream(TSHandler::init(&v));                                                \
    TSStream &v##_s = v##_stream.s

/* Wisp: var search: TerminalSearch = try .init(alloc, needle);
 *       defer search.deinit(&t); */
struct TSSearchHolder {
    TerminalSearch search;
    wisp::vt::Terminal *t;
    bool ok;
    TSSearchHolder(const char *needle, wisp::vt::Terminal *t_) : t(t_) {
        ok = TerminalSearch::init(talloc(), (const uint8_t *)needle, strlen(needle), &search);
    }
    ~TSSearchHolder() {
        if (ok) search.deinit(t);
    }
};

#define TSEARCH(var, needle, term)                                                                 \
    TSSearchHolder var##_holder((needle), &(term));                                                \
    ASSERT_TRUE(var##_holder.ok);                                                                  \
    TerminalSearch &var = var##_holder.search

/* Wisp: the pump loop that upstream repeats inline in several tests (and
 * factors into a `Pump` struct in the snapshot test). `.complete =>
 * unreachable` is unreachable because the `while` condition excludes it. */
static void tsPump(TerminalSearch *search, wisp::vt::Terminal *term) {
    while (search->status() != TerminalSearch::Status::complete) {
        switch (search->status()) {
        case TerminalSearch::Status::feed_required: search->feed(term, true); break;
        case TerminalSearch::Status::running: (void)search->tick(); break;
        case TerminalSearch::Status::complete: break; /* unreachable */
        }
    }
}

/* Wisp: `try testing.expect(try search.select(&t, .next, .if_needed))` */
#define SELECT_TRUE(search, term, which, how)                                                      \
    do {                                                                                           \
        bool _sel = false;                                                                         \
        ASSERT_TRUE((search).select(&(term), ScreenSearch::Select::which,                           \
                                    TerminalSearch::SelectScroll::how, &_sel));                    \
        ASSERT_TRUE(_sel);                                                                         \
    } while (0)

/* Wisp: `try testing.expect(!try search.select(&t, .next, .if_needed))` */
#define SELECT_FALSE(search, term, which, how)                                                     \
    do {                                                                                           \
        bool _sel = true;                                                                          \
        ASSERT_TRUE((search).select(&(term), ScreenSearch::Select::which,                           \
                                    TerminalSearch::SelectScroll::how, &_sel));                    \
        ASSERT_FALSE(_sel);                                                                         \
    } while (0)

/* Wisp: `_ = try search.select(&t, .next, .if_needed)` — the bool return is
 * the OutOfMemory channel and upstream discards the out-param. */
#define SELECT_DISCARD(search, term, which, how)                                                   \
    do {                                                                                           \
        bool _sel = false;                                                                         \
        ASSERT_TRUE((search).select(&(term), ScreenSearch::Select::which,                           \
                                    TerminalSearch::SelectScroll::how, &_sel));                    \
    } while (0)

/* Wisp: `try testing.expectEqual(n, (try search.viewportMatches()).len)` —
 * the slice is a pointer plus an out-length. */
#define EXPECT_VPM_LEN(expected, search)                                                           \
    do {                                                                                           \
        const Flattened *_p = NULL;                                                                \
        size_t _n = 0;                                                                             \
        ASSERT_TRUE((search).viewportMatches(&_p, &_n));                                           \
        ASSERT_EQ((size_t)(expected), _n);                                                         \
    } while (0)

/* Wisp: `search.activeScreenSearch().?.selectedMatch().?.startPin()` — the
 * Flattened is bound to a named local so the Pin does not reference a
 * destroyed temporary. */
static bool tsSelectedStartPin(TerminalSearch *search, Pin *out) {
    ScreenSearch *const ss = search->activeScreenSearch();
    if (ss == NULL) return false;
    Flattened hl;
    if (!ss->selectedMatch(&hl)) return false;
    *out = hl.startPin();
    return true;
}

/* ────────────────────────────────────────────────────────────────────── */

TEST(search_terminal, starts_feed_required_and_runs_to_complete) {
    TS_TERM(t, 10, 2);
    TS_STREAM(t);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    TSEARCH(search, "Fizz", t);

    /* A fresh search has never seen the terminal, so it must report
     * feed_required even though isComplete() is trivially true. */
    ASSERT_TRUE(search.isComplete());
    ASSERT_TRUE(TerminalSearch::Status::feed_required == search.status());

    /* Pump until complete. */
    tsPump(&search, &t);

    ScreenSearch *const screen_search = search.activeScreenSearch();
    ASSERT_NOT_NULL(screen_search);
    ASSERT_EQ((size_t)2, screen_search->matchesLen());
}

TEST(search_terminal, viewport_matches_are_cached_until_the_next_feed) {
    TS_TERM(t, 10, 10);
    TS_STREAM(t);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    TSEARCH(search, "Fizz", t);
    search.feed(&t, true);

    /* The sliding window drains on read, so a second read must return
     * the cached results rather than an empty list. */
    EXPECT_VPM_LEN(2, search);
    EXPECT_VPM_LEN(2, search);

    /* A feed that observes a change refreshes the cache. */
    t_s.nextSlice("\r\nFizz");
    search.feed(&t, true);
    EXPECT_VPM_LEN(3, search);
}

TEST(search_terminal, select_after_active_screen_removal) {
    TS_TERM(t, 20, 2);

    {
        bool ok = false;
        (void)t.switchScreen(ScreenSet::Key::alternate, &ok);
        ASSERT_TRUE(ok);
    }

    TSEARCH(search, "needle", t);
    search.feed(&t, false);
    ASSERT_TRUE(ScreenSet::Key::alternate == search.active_key);
    ASSERT_TRUE(search.screens.contains(ScreenSet::Key::alternate));

    {
        bool ok = false;
        (void)t.switchScreen(ScreenSet::Key::primary, &ok);
        ASSERT_TRUE(ok);
    }
    t.screens.remove(talloc(), ScreenSet::Key::alternate);

    /* The select must reconcile against the live ScreenSet before
     * touching any pins from the removed screen. */
    SELECT_DISCARD(search, t, next, if_needed);
    ASSERT_TRUE(ScreenSet::Key::primary == search.active_key);
    ASSERT_FALSE(search.screens.contains(ScreenSet::Key::alternate));
}

/* Whether the selected match is within the visible viewport rows.
 * pointFromPin only bounds-checks the top of the region, so rows
 * below the viewport must be rejected by the row count.
 *
 * Wisp: upstream's `const Visible = struct { fn check(...) }` inside the
 * test; C++ has no nested function, so it is a file-scope static. */
static bool tsVisibleCheck(wisp::vt::Terminal *term, TerminalSearch *s) {
    Pin pin;
    /* Wisp: upstream's `.?` on activeScreenSearch() and selectedMatch(). */
    const bool got = tsSelectedStartPin(s, &pin);
    assert(got);
    (void)got;
    PageList *const pages = &term->screens.active->pages;
    const Maybe<Point> pt = pages->pointFromPin(point::Tag::viewport, pin);
    if (!pt.has) return false;
    return pt.value.c.y < (uint32_t)pages->rows;
}

TEST(search_terminal, select_scrolls_the_viewport_only_when_needed) {
    TS_TERM_SB(t, 10, 2);
    TS_STREAM(t);
    t_s.nextSlice("Fizz\r\n");
    for (size_t i = 0; i < 30; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("Fizz");

    TSEARCH(search, "Fizz", t);
    tsPump(&search, &t);

    /* First match is at the bottom which is already visible: no scroll. */
    SELECT_TRUE(search, t, next, if_needed);
    {
        Pin pin;
        ASSERT_TRUE(tsSelectedStartPin(&search, &pin));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, pin),
                         Point::active(0, 1)));
    }
    ASSERT_TRUE(tsVisibleCheck(&t, &search));

    /* Second match is in scrollback: selecting it must scroll the
     * viewport so it becomes visible. */
    SELECT_TRUE(search, t, next, if_needed);
    ASSERT_TRUE(tsVisibleCheck(&t, &search));

    /* With scrolling disabled the viewport must stay where it is even
     * though the next selection (wrap back to the bottom) is not
     * visible. */
    SELECT_TRUE(search, t, next, none);
    ASSERT_FALSE(tsVisibleCheck(&t, &search));
}

TEST(search_terminal, deinit_after_the_terminal_is_gone) {
    /* Wisp: no holders here. Upstream deliberately deinitializes the
     * terminal before the search, so the order is written out by hand. */
    wisp::vt::Terminal t;
    {
        wisp::vt::Terminal::Options o((CellCountInt)10, (CellCountInt)2);
        o.max_scrollback_bytes = Maybe<size_t>(SIZE_MAX);
        ASSERT_TRUE(wisp::vt::Terminal::init(talloc(), o, &t));
    }

    TSStream stream(TSHandler::init(&t), tsStreamOptions());
    stream.nextSlice("Fizz\r\nBuzz\r\nFizz");

    /* Run to complete and select a match so the search holds tracked
     * pins within the terminal's page storage. */
    TerminalSearch search;
    ASSERT_TRUE(TerminalSearch::init(talloc(), (const uint8_t *)"Fizz", 4, &search));
    tsPump(&search, &t);
    SELECT_TRUE(search, t, next, none);

    /* Deinitialize the terminal first. The pins died with the page
     * storage, so deinit with a null terminal must free only
     * search-owned memory without touching the terminal. */
    stream.deinit();
    t.deinit(talloc());
    search.deinit(NULL);
}

TEST(search_terminal, no_matches_selects_nothing) {
    TS_TERM(t, 10, 2);

    TSEARCH(search, "Fizz", t);
    search.feed(&t, true);
    SELECT_FALSE(search, t, next, if_needed);
    SELECT_FALSE(search, t, prev, if_needed);
}

/* Wisp: omitted upstream test — "feed after complete discovers prepended
 * snapshot history". It needs `snapshot.encode` and `snapshot.Decoder` from
 * `src/terminal/snapshot/main.zig`, which is not ported in Wisp. Not a
 * build-flag omission. */

TEST(search_terminal, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
