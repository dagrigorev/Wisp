/* Transliterated from the test blocks in Ghostty
 * src/terminal/search/viewport.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * All 6 upstream `test "..."` blocks are ported. None are gated on a
 * build flag, so none are omitted.
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place by
 * VPStreamHolder — the same scaffolding the sliding_window port test uses.
 */

#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "vt_screen_test_helpers.hpp"

#include "../terminal/stream_terminal.hpp"
#include "../vt/highlight.hpp"
#include "../vt/search/viewport.hpp"
#include "../vt/terminal.hpp"

typedef wisp::vt::search::ViewportSearch ViewportSearch;
typedef wisp::vt::PageList::Node Node;
typedef wisp::vt::highlight::Flattened Flattened;
typedef wisp::vt::highlight::Untracked Untracked;

typedef wisp::terminal::stream_terminal::Handler VPHandler;
typedef wisp::terminal::stream::Stream<VPHandler> VPStream;

/* Wisp: var t: Terminal = try .init(io, alloc, .{...}); defer t.deinit(alloc) */
struct VPTermHolder {
    wisp::vt::Terminal t;
    bool ok;
    VPTermHolder(unsigned cols, unsigned rows) {
        wisp::vt::Terminal::Options o((wisp::vt::size::CellCountInt)cols,
                                      (wisp::vt::size::CellCountInt)rows);
        ok = wisp::vt::Terminal::init(talloc(), o, &t);
    }
    ~VPTermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: var s = t.vtStream(); defer s.deinit(); */
struct VPStreamHolder {
    VPStream s;
    explicit VPStreamHolder(const VPHandler &h) : s(h, streamOptions()) {}
    ~VPStreamHolder() { s.deinit(); }

    static VPStream::Options streamOptions() {
        VPStream::Options o;
        o.allocator = true;
        return o;
    }
};

#define VP_TERM(v, c, r)                                                                           \
    VPTermHolder v##_holder((c), (r));                                                             \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t

#define VP_STREAM(v)                                                                               \
    VPStreamHolder v##_stream(VPHandler::init(&v));                                                \
    VPStream &v##_s = v##_stream.s

/* Wisp: var search: ViewportSearch = try .init(alloc, needle);
 *       defer search.deinit(); */
struct SearchHolder {
    ViewportSearch v;
    bool ok;
    SearchHolder(wisp::zigstd::Allocator alloc, const char *needle) {
        ok = ViewportSearch::init(alloc, (const uint8_t *)needle, strlen(needle), &v);
    }
    ~SearchHolder() {
        if (ok) v.deinit();
    }
};

#define SEARCH_IN(var, alloc, needle)                                                              \
    SearchHolder var##_holder((alloc), (needle));                                                  \
    ASSERT_TRUE(var##_holder.ok);                                                                  \
    ViewportSearch &var = var##_holder.v

#define SEARCH(var, needle) SEARCH_IN(var, talloc(), needle)

/* Wisp: `try testing.expect(try search.update(&pages))` — the
 * Allocator.Error is the bool return, the viewport-changed result the
 * out-param. */
#define EXPECT_UPDATE(search, pages)                                                               \
    do {                                                                                           \
        bool _changed = false;                                                                     \
        ASSERT_TRUE((search).update(&(pages), &_changed));                                         \
        ASSERT_TRUE(_changed);                                                                     \
    } while (0)

/* Wisp: `try testing.expect(!try search.update(&pages))` */
#define EXPECT_NO_UPDATE(search, pages)                                                            \
    do {                                                                                           \
        bool _changed = true;                                                                      \
        ASSERT_TRUE((search).update(&(pages), &_changed));                                         \
        ASSERT_FALSE(_changed);                                                                    \
    } while (0)

/* Wisp: `const h = search.next().?` */
#define NEXT(h, search)                                                                            \
    Flattened h;                                                                                   \
    ASSERT_TRUE((search).next(&h))

/* Wisp: `try testing.expect(search.next() == null)` */
#define NO_NEXT(search)                                                                            \
    do {                                                                                           \
        Flattened _h;                                                                              \
        ASSERT_FALSE((search).next(&_h));                                                          \
    } while (0)

/* Wisp: expectEqual(point.Point{ .active = .{ .x = X, .y = Y } },
 *                   pages.pointFromPin(.active, pin).?) */
#define EXPECT_PT_ACTIVE(pages, pin, X, Y)                                                         \
    ASSERT_TRUE(ptEq((pages).pointFromPin(point::Tag::active, (pin)),                              \
                     Point::active((wisp::vt::size::CellCountInt)(X), (uint32_t)(Y))))

/* Wisp: expectEqual(point.Point{ .screen = .{ .x = X, .y = Y } },
 *                   pages.pointFromPin(.screen, pin).?) */
#define EXPECT_PT_SCREEN(pages, pin, X, Y)                                                         \
    ASSERT_TRUE(ptEq((pages).pointFromPin(point::Tag::screen, (pin)),                              \
                     Point::screen((wisp::vt::size::CellCountInt)(X), (uint32_t)(Y))))

/* ────────────────────────────────────────────────────────────────────── */

TEST(search_viewport, simple_search) {
    VP_TERM(t, 10, 10);
    VP_STREAM(t);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz");
    EXPECT_UPDATE(search, t.screens.active->pages);

    /* Viewport contains active so update should always re-search. */
    EXPECT_UPDATE(search, t.screens.active->pages);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.start, 0, 0);
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.end, 3, 0);
    }
    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.start, 0, 2);
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.end, 3, 2);
    }
    NO_NEXT(search);
}

TEST(search_viewport, fingerprint_detects_node_generation_changes) {
    VP_TERM(t, 10, 10);

    SEARCH(search, "needle");
    search.active_dirty = wisp::vt::Maybe<bool>(false);

    EXPECT_UPDATE(search, t.screens.active->pages);
    EXPECT_NO_UPDATE(search, t.screens.active->pages);

    const ViewportSearch::Fingerprint::Entry old_entry = search.fingerprint.entries.items[0];
    Node *node = t.screens.active->pages.pages.first;
    /* Wisp: ASSERT_EQ formats through long, which truncates a pointer. */
    ASSERT_TRUE(node == old_entry.node);
    const uint64_t original_serial = node->serial;
    /* Wisp: defer node.serial = original_serial; */
    struct SerialRestore {
        Node *node;
        uint64_t serial;
        ~SerialRestore() { node->serial = serial; }
    } serial_restore = {node, original_serial};
    (void)serial_restore;
    node->serial += 1;

    /* The viewport still contains the same pointer, but its generation has
     * changed, so the cached window must be rebuilt. */
    EXPECT_UPDATE(search, t.screens.active->pages);
    const ViewportSearch::Fingerprint::Entry new_entry = search.fingerprint.entries.items[0];
    ASSERT_TRUE(old_entry.node == new_entry.node);
    ASSERT_TRUE(old_entry.serial != new_entry.serial);
}

TEST(search_viewport, fingerprint_only_allocates_on_changes) {
    VP_TERM(t, 10, 10);

    /* Wisp: testing.FailingAllocator.init(alloc, .{}) — the default
     * fail_index is maxInt(usize), i.e. never fail. */
    wisp::zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const wisp::zigstd::Allocator failing_alloc = failing.allocator();

    SEARCH_IN(search, failing_alloc, "needle");
    search.active_dirty = wisp::vt::Maybe<bool>(false);

    EXPECT_UPDATE(search, t.screens.active->pages);

    /* Fail the next allocation. An unchanged viewport with active dirty
     * tracking disabled should reuse the existing fingerprint and return
     * unchanged before rebuilding the search window, so this update should
     * succeed without reporting a viewport change. */
    failing.fail_index = failing.alloc_index;
    EXPECT_NO_UPDATE(search, t.screens.active->pages);
}

TEST(search_viewport, clear_screen_and_search) {
    VP_TERM(t, 10, 10);
    VP_STREAM(t);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz");
    EXPECT_UPDATE(search, t.screens.active->pages);

    t_s.nextSlice("\x1b" "[2J"); /* Clear screen */
    t_s.nextSlice("\x1b" "[H");  /* Move cursor home */
    t_s.nextSlice("Buzz\r\nFizz\r\nBuzz");
    EXPECT_UPDATE(search, t.screens.active->pages);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.start, 0, 1);
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.end, 3, 1);
    }
    NO_NEXT(search);
}

TEST(search_viewport, clear_screen_and_search_dirty_tracking) {
    VP_TERM(t, 10, 10);
    VP_STREAM(t);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz");

    /* Turn on dirty tracking */
    search.active_dirty = wisp::vt::Maybe<bool>(false);

    /* Should update since we've never searched before */
    EXPECT_UPDATE(search, t.screens.active->pages);

    /* Should not update since nothing changed */
    EXPECT_NO_UPDATE(search, t.screens.active->pages);

    t_s.nextSlice("\x1b" "[2J"); /* Clear screen */
    t_s.nextSlice("\x1b" "[H");  /* Move cursor home */
    t_s.nextSlice("Buzz\r\nFizz\r\nBuzz");

    /* Should still not update since active area isn't dirty */
    EXPECT_NO_UPDATE(search, t.screens.active->pages);

    /* Mark */
    search.active_dirty = wisp::vt::Maybe<bool>(true);
    EXPECT_UPDATE(search, t.screens.active->pages);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.start, 0, 1);
        EXPECT_PT_ACTIVE(t.screens.active->pages, sel.end, 3, 1);
    }
    NO_NEXT(search);
}

TEST(search_viewport, history_search_no_active_area) {
    VP_TERM(t, 10, 2);
    VP_STREAM(t);

    /* Fill up first page */
    const wisp::vt::size::CellCountInt first_page_rows =
        t.screens.active->pages.pages.first->capacity().rows;
    t_s.nextSlice("Fizz\r\n");
    for (size_t i = 1; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    ASSERT_TRUE(t.screens.active->pages.pages.first == t.screens.active->pages.pages.last);

    /* Create second page */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(t.screens.active->pages.pages.first != t.screens.active->pages.pages.last);
    t_s.nextSlice("Buzz\r\nFizz");

    t.scrollViewport(wisp::vt::Terminal::ScrollViewport::makeTop());

    SEARCH(search, "Fizz");
    EXPECT_UPDATE(search, t.screens.active->pages);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        EXPECT_PT_SCREEN(t.screens.active->pages, sel.start, 0, 0);
        EXPECT_PT_SCREEN(t.screens.active->pages, sel.end, 3, 0);
    }
    NO_NEXT(search);

    /* Viewport doesn't contain active */
    EXPECT_NO_UPDATE(search, t.screens.active->pages);
    NO_NEXT(search);
}

TEST(search_viewport, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
