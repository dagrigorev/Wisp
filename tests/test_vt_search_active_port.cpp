/* Transliterated from the test blocks in Ghostty
 * src/terminal/search/active.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Both upstream `test "..."` blocks are ported. Neither is gated on a
 * build flag, so none are omitted.
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place by
 * StreamHolder as in the sliding_window test.
 */

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"
#include "vt_screen_test_helpers.hpp"

#include "../terminal/stream_terminal.hpp"
#include "../vt/highlight.hpp"
#include "../vt/search/active.hpp"

typedef wisp::vt::search::ActiveSearch ActiveSearch;
typedef wisp::vt::PageList::Node Node;
typedef wisp::vt::highlight::Flattened Flattened;
typedef wisp::vt::highlight::Untracked Untracked;

typedef wisp::terminal::stream_terminal::Handler ASHandler;
typedef wisp::terminal::stream::Stream<ASHandler> ASStream;

/* Wisp: var t: Terminal = try .init(io, alloc, .{...}); defer t.deinit(alloc) */
struct ASTermHolder {
    wisp::vt::Terminal t;
    bool ok;
    ASTermHolder(unsigned cols, unsigned rows) {
        wisp::vt::Terminal::Options o((wisp::vt::size::CellCountInt)cols,
                                      (wisp::vt::size::CellCountInt)rows);
        ok = wisp::vt::Terminal::init(talloc(), o, &t);
    }
    ~ASTermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: var s = t.vtStream(); defer s.deinit(); */
struct ASStreamHolder {
    ASStream s;
    explicit ASStreamHolder(const ASHandler &h) : s(h, streamOptions()) {}
    ~ASStreamHolder() { s.deinit(); }

    static ASStream::Options streamOptions() {
        ASStream::Options o;
        o.allocator = true;
        return o;
    }
};

#define AS_TERM(v, c, r)                                                                           \
    ASTermHolder v##_holder((c), (r));                                                              \
    ASSERT_TRUE(v##_holder.ok);                                                                     \
    wisp::vt::Terminal &v = v##_holder.t;                                                           \
    ASStreamHolder v##_stream(ASHandler::init(&v));                                                 \
    ASStream &v##_s = v##_stream.s

/* Wisp: var search: ActiveSearch = try .init(alloc, needle);
 *       defer search.deinit(); */
struct ActiveSearchHolder {
    ActiveSearch search;
    bool ok;
    ActiveSearchHolder(const uint8_t *needle, size_t len) {
        ok = ActiveSearch::init(talloc(), needle, len, &search);
    }
    ~ActiveSearchHolder() {
        if (ok) search.deinit();
    }
};

#define ACTIVE_SEARCH(var, needle)                                                                 \
    ActiveSearchHolder var##_holder((const uint8_t *)(needle), strlen(needle));                     \
    ASSERT_TRUE(var##_holder.ok);                                                                   \
    ActiveSearch &var = var##_holder.search

/* Wisp: `_ = try search.update(&pages)` — the returned node is discarded. */
static bool updateSearch(ActiveSearch &search, const wisp::vt::PageList *pages) {
    Node *node = nullptr;
    return search.update(pages, &node);
}

#define UPDATE(search, pages) ASSERT_TRUE(updateSearch((search), &(pages)))

/* Wisp: `const h = search.next().?` */
#define NEXT(h, search)                                                                            \
    Flattened h;                                                                                    \
    ASSERT_TRUE((search).next(&h))

/* Wisp: `try testing.expect(search.next() == null)` */
#define NO_NEXT(search)                                                                            \
    do {                                                                                            \
        Flattened _h;                                                                                \
        ASSERT_FALSE((search).next(&_h));                                                            \
    } while (0)

/* Wisp: expectEqual(point.Point{ .active = .{ .x = X, .y = Y } },
 *                   pages.pointFromPin(.active, pin)) */
#define EXPECT_PT(pages, pin, X, Y)                                                                \
    ASSERT_TRUE(ptEq((pages).pointFromPin(point::Tag::active, (pin)),                               \
                     Point::active((wisp::vt::size::CellCountInt)(X), (uint32_t)(Y))))

/* ────────────────────────────────────────────────────────────────────── */

TEST(search_active, simple_search) {
    AS_TERM(t, 10, 10);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    ACTIVE_SEARCH(search, "Fizz");
    Screen *screen = t.screens.active;
    UPDATE(search, screen->pages);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT(screen->pages, sel.start, 0, 0);
        EXPECT_PT(screen->pages, sel.end, 3, 0);
    }
    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT(screen->pages, sel.start, 0, 2);
        EXPECT_PT(screen->pages, sel.end, 3, 2);
    }
    NO_NEXT(search);
}

TEST(search_active, clear_screen_and_search) {
    AS_TERM(t, 10, 10);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    ACTIVE_SEARCH(search, "Fizz");
    Screen *screen = t.screens.active;
    UPDATE(search, screen->pages);

    t_s.nextSlice("\x1b" "[2J"); /* Clear screen */
    t_s.nextSlice("\x1b" "[H");  /* Move cursor home */
    t_s.nextSlice("Buzz\r\nFizz\r\nBuzz");
    UPDATE(search, screen->pages);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT(screen->pages, sel.start, 0, 1);
        EXPECT_PT(screen->pages, sel.end, 3, 1);
    }
    NO_NEXT(search);
}

TEST(search_active, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
