/* Transliterated from the test blocks in Ghostty
 * src/terminal/search/pagelist.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * All 10 upstream `test "..."` blocks are ported. None are gated on a
 * build flag, so none are omitted.
 *
 * Mapping:
 *   var t: Terminal = try .init(io, alloc, .{...})   TermHolder
 *   var s = t.vtStream(); defer s.deinit()           StreamHolder
 *   try search.feed()                                FEED_TRUE / FEED_FALSE
 *   search.next().?                                  NEXT(h, search)
 *   search.next() == null                            NO_NEXT(search)
 *   try p.grow() == null                             GROW_NULL(p)
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place by
 * StreamHolder as in the sliding_window/formatter tests.
 */

#include <stdint.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "vt_screen_test_helpers.hpp"

#include "../terminal/stream_terminal.hpp"
#include "../vt/highlight.hpp"
#include "../vt/search/pagelist.hpp"

typedef wisp::vt::search::PageListSearch PageListSearch;
typedef wisp::vt::PageList::Node Node;
typedef wisp::vt::PageList::Options PLOptions;
typedef wisp::vt::highlight::Flattened Flattened;
typedef wisp::vt::highlight::Untracked Untracked;

typedef wisp::vt::page::Capacity Capacity;

typedef wisp::terminal::stream_terminal::Handler PLSHandler;
typedef wisp::terminal::stream::Stream<PLSHandler> PLSStream;

/* Wisp: var t: Terminal = try .init(io, alloc, .{...}); defer t.deinit(alloc) */
struct PLSTermHolder {
    wisp::vt::Terminal t;
    bool ok;
    PLSTermHolder(unsigned cols, unsigned rows, Maybe<size_t> max_scrollback_bytes) {
        wisp::vt::Terminal::Options o((wisp::vt::size::CellCountInt)cols,
                                      (wisp::vt::size::CellCountInt)rows);
        o.max_scrollback_bytes = max_scrollback_bytes;
        ok = wisp::vt::Terminal::init(talloc(), o, &t);
    }
    ~PLSTermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: var s = t.vtStream(); defer s.deinit(); */
struct PLSStreamHolder {
    PLSStream s;
    explicit PLSStreamHolder(const PLSHandler &h) : s(h, streamOptions()) {}
    ~PLSStreamHolder() { s.deinit(); }

    static PLSStream::Options streamOptions() {
        PLSStream::Options o;
        o.allocator = true;
        return o;
    }
};

/* Wisp: the default max_scrollback_bytes of Terminal.Options. Upstream's
 * tests that don't name it get the struct default. */
static Maybe<size_t> defaultScrollbackBytes() { return Maybe<size_t>((size_t)10000); }

#define PLS_TERM(v, c, r)                                                                          \
    PLSTermHolder v##_holder((c), (r), defaultScrollbackBytes());                                  \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t;                                                          \
    PLSStreamHolder v##_stream(PLSHandler::init(&v));                                              \
    PLSStream &v##_s = v##_stream.s

#define PLS_TERM_SB(v, c, r, sb)                                                                   \
    PLSTermHolder v##_holder((c), (r), Maybe<size_t>((size_t)(sb)));                                \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    wisp::vt::Terminal &v = v##_holder.t;                                                          \
    PLSStreamHolder v##_stream(PLSHandler::init(&v));                                              \
    PLSStream &v##_s = v##_stream.s

/* Wisp: var p: PageList = try .init(alloc, .{...}); defer p.deinit(); */
struct PLSListHolder {
    PageList s;
    bool ok;
    explicit PLSListHolder(const PLOptions &o) { ok = PageList::init(talloc(), o, &s); }
    ~PLSListHolder() {
        if (ok) s.deinit();
    }
    PageList *operator->() { return &s; }
    PageList &operator*() { return s; }
};

/* Wisp: var search: PageListSearch = try .init(...); defer search.deinit(); */
struct SearchHolder {
    PageListSearch search;
    bool ok;
    SearchHolder(const char *needle, PageList *list, Node *start) {
        ok = PageListSearch::init(talloc(), (const uint8_t *)needle, strlen(needle), list, start,
                                  &search);
    }
    ~SearchHolder() {
        if (ok) search.deinit();
    }
};

#define SEARCH(var, needle, list, start)                                                           \
    SearchHolder var##_holder((needle), (list), (start));                                          \
    ASSERT_TRUE(var##_holder.ok);                                                                  \
    PageListSearch &var = var##_holder.search

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

/* Wisp: `try testing.expect(try search.feed())` — the bool return is the
 * OutOfMemory channel, the out-param is upstream's value. */
#define FEED_TRUE(search)                                                                          \
    do {                                                                                           \
        bool _fed = false;                                                                         \
        ASSERT_TRUE((search).feed(&_fed));                                                         \
        ASSERT_TRUE(_fed);                                                                         \
    } while (0)

/* Wisp: `try testing.expect(!try search.feed())` */
#define FEED_FALSE(search)                                                                         \
    do {                                                                                           \
        bool _fed = true;                                                                          \
        ASSERT_TRUE((search).feed(&_fed));                                                         \
        ASSERT_FALSE(_fed);                                                                        \
    } while (0)

/* Wisp: `try testing.expect(try p.grow() == null)` */
#define GROW_NULL(p)                                                                               \
    do {                                                                                           \
        Node *_n = (Node *)1;                                                                      \
        ASSERT_TRUE((p).grow(&_n));                                                                \
        ASSERT_TRUE(_n == nullptr);                                                                \
    } while (0)

/* Wisp: `(try p.grow()).?` */
static Node *growNode(PageList &p) {
    Node *n = nullptr;
    if (!p.grow(&n)) return nullptr;
    return n;
}

/* Wisp: expectEqual on PageList.MemoryStats, which has no operator==. */
static bool statsEq(const PageList::MemoryStats &a, const PageList::MemoryStats &b) {
    return a.resident_pages == b.resident_pages && a.compressed_pages == b.compressed_pages &&
           a.raw_bytes == b.raw_bytes && a.resident_raw_bytes == b.resident_raw_bytes &&
           a.decommitted_raw_bytes == b.decommitted_raw_bytes &&
           a.resident_backing_bytes == b.resident_backing_bytes &&
           a.encoded_bytes == b.encoded_bytes;
}

/* ────────────────────────────────────────────────────────────────────── */

TEST(search_pagelist, simple_search) {
    PLS_TERM(t, 10, 10);
    t_s.nextSlice("Fizz\r\nBuzz\r\nFizz\r\nBang");

    SEARCH(search, "Fizz", &t.screens.active->pages, t.screens.active->pages.pages.last);

    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.start),
                         Point::active(0, 2)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.end),
                         Point::active(3, 2)));
    }
    {
        NEXT(h, search);
        const Untracked sel = h.untracked();
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.start),
                         Point::active(0, 0)));
        ASSERT_TRUE(ptEq(t.screens.active->pages.pointFromPin(point::Tag::active, sel.end),
                         Point::active(3, 0)));
    }
    NO_NEXT(search);

    /* We should not be able to feed since we have one page */
    FEED_FALSE(search);
}

TEST(search_pagelist, feed_multiple_pages_with_matches) {
    PLS_TERM(t, 10, 10);

    /* Fill up first page */
    const size::CellCountInt first_page_rows = t.screens.active->pages.pages.first->capacity().rows;
    for (size_t i = 0; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("Fizz");
    ASSERT_TRUE(t.screens.active->pages.pages.first == t.screens.active->pages.pages.last);

    /* Create second page */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(t.screens.active->pages.pages.first != t.screens.active->pages.pages.last);
    t_s.nextSlice("Buzz\r\nFizz");

    SEARCH(search, "Fizz", &t.screens.active->pages, t.screens.active->pages.pages.last);

    /* First match on the last page */
    {
        NEXT(sel1, search);
        (void)sel1;
    }
    NO_NEXT(search);

    /* Feed should succeed and load the first page */
    FEED_TRUE(search);

    /* Now we should find the match on the first page */
    {
        NEXT(sel2, search);
        (void)sel2;
    }
    NO_NEXT(search);

    /* No more pages to feed */
    FEED_FALSE(search);
}

TEST(search_pagelist, feed_multiple_pages_no_matches) {
    PLS_TERM(t, 10, 10);

    /* Fill up first page */
    const size::CellCountInt first_page_rows = t.screens.active->pages.pages.first->capacity().rows;
    for (size_t i = 0; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("Hello");

    /* Create second page */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(t.screens.active->pages.pages.first != t.screens.active->pages.pages.last);
    t_s.nextSlice("World");

    SEARCH(search, "Nope", &t.screens.active->pages, t.screens.active->pages.pages.last);

    /* No matches on last page */
    NO_NEXT(search);

    /* Feed first page */
    FEED_TRUE(search);

    /* Still no matches */
    NO_NEXT(search);

    /* No more pages */
    FEED_FALSE(search);
}

TEST(search_pagelist, feed_iteratively_through_multiple_matches) {
    PLS_TERM(t, 80, 24);

    const size::CellCountInt first_page_rows = t.screens.active->pages.pages.first->capacity().rows;

    /* Fill first page with a match at the end */
    for (size_t i = 0; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("Page1Test");
    ASSERT_TRUE(t.screens.active->pages.pages.first == t.screens.active->pages.pages.last);

    /* Create second page with a match */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(t.screens.active->pages.pages.first != t.screens.active->pages.pages.last);
    t_s.nextSlice("Page2Test");

    SEARCH(search, "Test", &t.screens.active->pages, t.screens.active->pages.pages.last);

    /* Match on page 2 */
    {
        NEXT(h, search);
        (void)h;
    }
    NO_NEXT(search);

    /* Feed page 1 */
    FEED_TRUE(search);
    {
        NEXT(h, search);
        (void)h;
    }
    NO_NEXT(search);

    /* No more pages */
    FEED_FALSE(search);
}

TEST(search_pagelist, feed_with_match_spanning_page_boundary) {
    PLS_TERM(t, 80, 24);

    const size::CellCountInt first_page_rows = t.screens.active->pages.pages.first->capacity().rows;

    /* Fill first page ending with "Te" */
    for (size_t i = 0; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    for (size_t i = 0; i < (size_t)t.screens.active->pages.cols - 2; i++) t_s.nextSlice("x");
    t_s.nextSlice("Te");
    ASSERT_TRUE(t.screens.active->pages.pages.first == t.screens.active->pages.pages.last);

    /* Second page starts with "st" */
    t_s.nextSlice("st");
    ASSERT_TRUE(t.screens.active->pages.pages.first != t.screens.active->pages.pages.last);

    SEARCH(search, "Test", &t.screens.active->pages, t.screens.active->pages.pages.last);

    /* No complete match on last page alone (only has "st") */
    NO_NEXT(search);

    /* Feed first page - this should give us enough data to find "Test" */
    FEED_TRUE(search);

    /* Should find the spanning match */
    NEXT(h, search);
    const Untracked sel = h.untracked();
    ASSERT_TRUE(sel.start.node != sel.end.node);
    {
        EXPECT_STR("Test", t.screens.active->selectionString(Screen::SelectionString(
                               Selection::init(sel.start, sel.end, false), false)));
    }

    /* No more matches */
    NO_NEXT(search);

    /* No more pages */
    FEED_FALSE(search);
}

TEST(search_pagelist, compressed_history_match_spanning_page_boundary_remains_compressed) {
    PLS_TERM_SB(t, 80, 24, 10 * 1024 * 1024);

    PageList *pages = &t.screens.active->pages;
    Node *const first = pages->pages.first;
    const size::CellCountInt first_page_rows = first->capacity().rows;

    /* Put half the needle on either side of a soft-wrapped page boundary. */
    for (size_t i = 0; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    for (size_t i = 0; i < (size_t)pages->cols - 2; i++) t_s.nextSlice("x");
    t_s.nextSlice("Test");
    Node *const second = first->next;
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(second == pages->pages.last);

    /* Move both matching pages completely into history. The third page holds
     * the active area, making the first two eligible for compression. */
    while (pages->pages.last == second) t_s.nextSlice("\r\n");
    for (size_t i = 0; i < (size_t)pages->rows; i++) t_s.nextSlice("\r\n");
    ASSERT_TRUE(pages->getTopLeft(point::Tag::active).node != second);

    (void)pages->compress(PageList::CompressMode::full);
    ASSERT_TRUE(Node::Storage::compressed == first->storage());
    ASSERT_TRUE(Node::Storage::compressed == second->storage());
    const PageList::MemoryStats compressed = pages->memoryStats();
    ASSERT_TRUE(compressed.compressed_pages >= 2);

    SEARCH(search, "Test", pages, pages->pages.last);

    /* Drain and feed incrementally until the boundary match is available. The
     * exact number of feeds depends on how much blank-page text the formatter
     * trims, so bound progress by the number of pages rather than encoding
     * that implementation detail in the test. */
    Maybe<Flattened> found;
    for (size_t i = 0; i < pages->totalPages() + 1; i++) {
        Flattened match;
        if (search.next(&match)) {
            found = Maybe<Flattened>(match);
            break;
        }
        bool fed = false;
        ASSERT_TRUE(search.feed(&fed));
        if (!fed) break;
    }
    ASSERT_TRUE(found.has);

    const Untracked match = found.value.untracked();
    ASSERT_TRUE(first == match.start.node);
    ASSERT_TRUE(second == match.end.node);
    ASSERT_EQ((size::CellCountInt)(pages->cols - 2), match.start.x);
    ASSERT_EQ((size::CellCountInt)1, match.end.x);

    /* Search owns formatted text and coordinate maps, not page snapshots. The
     * original raw mappings therefore remain discarded after matching. */
    ASSERT_TRUE(statsEq(compressed, pages->memoryStats()));
    ASSERT_TRUE(Node::Storage::compressed == first->storage());
    ASSERT_TRUE(Node::Storage::compressed == second->storage());
}

TEST(search_pagelist, feed_with_match_spanning_page_boundary_with_newline) {
    PLS_TERM(t, 80, 24);

    const size::CellCountInt first_page_rows = t.screens.active->pages.pages.first->capacity().rows;

    /* Fill first page ending with "Te" */
    for (size_t i = 0; i < (size_t)first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    for (size_t i = 0; i < (size_t)t.screens.active->pages.cols - 2; i++) t_s.nextSlice("x");
    t_s.nextSlice("Te");
    ASSERT_TRUE(t.screens.active->pages.pages.first == t.screens.active->pages.pages.last);

    /* Second page starts with "st" */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(t.screens.active->pages.pages.first != t.screens.active->pages.pages.last);
    t_s.nextSlice("st");

    SEARCH(search, "Test", &t.screens.active->pages, t.screens.active->pages.pages.last);

    /* Should not find any matches since we broke with an explicit newline. */
    NO_NEXT(search);
    FEED_TRUE(search);
    NO_NEXT(search);
    FEED_FALSE(search);
}

TEST(search_pagelist, feed_with_pruned_page) {
    /* Zero here forces minimum max size to effectively two pages. */
    PLSListHolder p(PLOptions(80, 24, Maybe<size_t>((size_t)0)));
    ASSERT_TRUE(p.ok);

    /* Grow to capacity */
    Node *const page1_node = p->pages.last;
    Page *const page1 = page1_node->page();
    /* Wisp: Zig evaluates the `0..n` range once; snapshot the count because
     * `page1.size.rows` grows on every iteration. */
    const size_t page1_grow = (size_t)(page1->capacity.rows - page1->size.rows);
    for (size_t i = 0; i < page1_grow; i++) GROW_NULL(*p);

    /* Grow and allocate one more page. Then fill that page up. */
    Node *const page2_node = growNode(*p);
    ASSERT_TRUE(page2_node != nullptr);
    Page *const page2 = page2_node->page();
    const size_t page2_grow = (size_t)(page2->capacity.rows - page2->size.rows);
    for (size_t i = 0; i < page2_grow; i++) GROW_NULL(*p);

    /* Setup search and feed until we can't */
    SEARCH(search, "Test", &(*p), p->pages.last);
    FEED_TRUE(search);
    FEED_FALSE(search);

    /* Next should create a new page, but it should reuse our first
     * page since we're at max size. */
    Node *const new_ = growNode(*p);
    ASSERT_TRUE(new_ != nullptr);
    ASSERT_TRUE(p->pages.last == new_);

    /* Our first should now be page2 and our last should be page1 */
    ASSERT_TRUE(page2_node == p->pages.first);
    ASSERT_TRUE(page1_node == p->pages.last);

    /* Feed should still do nothing */
    FEED_FALSE(search);
}

TEST(search_pagelist, feed_keeps_its_tracked_pin_within_a_shorter_page) {
    PLSListHolder pages(PLOptions(10, 2));
    ASSERT_TRUE(pages.ok);

    Node *const first = pages->pages.first;
    while (first->rows() < first->capacity().rows) {
        Node *ignored = nullptr;
        ASSERT_TRUE(pages->grow(&ignored));
    }

    Node *const second = growNode(*pages);
    ASSERT_TRUE(second != nullptr);
    while (second->rows() < second->capacity().rows) {
        Node *ignored = nullptr;
        ASSERT_TRUE(pages->grow(&ignored));
    }

    ASSERT_TRUE(PageList::SplitError::none == pages->split(Pin(first, 1, 0)));
    Node *const shorter = first->next;
    ASSERT_TRUE(shorter != nullptr);
    ASSERT_TRUE(shorter->rows() < second->rows());

    SEARCH(search, "x", &(*pages), second);

    FEED_TRUE(search);
    ASSERT_TRUE(shorter == search.pin->node);
    ASSERT_TRUE(pages->pinIsValid(*search.pin));
}

TEST(search_pagelist, feed_discovers_pages_prepended_after_exhaustion) {
    PLS_TERM_SB(t, 10, 10, SIZE_MAX);
    t_s.nextSlice("Fizz");

    /* Exhaust the list: one match on the only page and nothing left to feed. */
    PageList *pages = &t.screens.active->pages;
    SEARCH(search, "Fizz", pages, pages->pages.last);
    {
        NEXT(h, search);
        (void)h;
    }
    NO_NEXT(search);
    FEED_FALSE(search);

    /* Prepend an older page the way incremental snapshot history restore
     * does. The frontier pin names the page that used to be first, and
     * finalize keeps that node's identity, so the pin now has a `prev`. */
    Node *const old_first = pages->pages.first;
    {
        PageList::PageAllocation allocation;
        ASSERT_TRUE(pages->allocatePage(Capacity(10, 2), &allocation));
        /* Wisp: defer allocation.deinit(); */
        struct AllocGuard {
            PageList::PageAllocation *a;
            ~AllocGuard() { a->deinit(); }
        } guard = {&allocation};
        (void)guard;
        Page *const page = allocation.page();
        page->size.rows = 2;
        {
            const char *fizz = "Fizz";
            for (size_t x = 0; x < 4; x++) {
                *page->getRowAndCell(x, 1).cell = Cell::init((uint32_t)(uint8_t)fizz[x]);
            }
        }
        ASSERT_TRUE(PageList::PageAllocation::FinalizeError::none ==
                    allocation.finalize(PageList::PageAllocation::Location::prepend));
    }
    Node *const prepended = pages->pages.first;
    ASSERT_TRUE(prepended != old_first);
    ASSERT_TRUE(old_first == search.pin->node);

    /* Feeding resumes into the restored history and finds its match. */
    FEED_TRUE(search);
    ASSERT_TRUE(prepended == search.pin->node);
    NEXT(h, search);
    const Untracked sel = h.untracked();
    ASSERT_TRUE(prepended == sel.start.node);
    ASSERT_TRUE(prepended == sel.end.node);
    ASSERT_EQ((size::CellCountInt)1, sel.start.y);
    ASSERT_EQ((size::CellCountInt)0, sel.start.x);
    ASSERT_EQ((size::CellCountInt)3, sel.end.x);
    NO_NEXT(search);
    FEED_FALSE(search);
}

TEST(search_pagelist, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
