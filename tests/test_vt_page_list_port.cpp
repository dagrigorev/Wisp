/* Transliterated from the test blocks in Ghostty src/terminal/PageList.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 *
 * Mapping:
 *   try init(alloc, .{...})        ASSERT_TRUE(PageList::init(alloc, opts, &s))
 *   defer s.deinit()               ListHolder (deinit on scope exit)
 *   s.pin(pt).?                    s.pin(pt).value
 *   expectError(E, f)              ASSERT_TRUE(f == ...::E)
 */

#include <string.h>

#include "test_helpers.h"
#include "../vt/page_list.hpp"
#include "../zigstd/random.hpp"

using namespace wisp;
using namespace wisp::vt;

typedef PageList::Pin Pin;
typedef PageList::Node Node;
typedef PageList::Viewport Viewport;
typedef PageList::Options Options;
typedef PageList::Builder Builder;
typedef PageList::PageAllocation PageAllocation;
typedef page::Page Page;
typedef page::Capacity Capacity;
typedef point::Point Point;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

struct ListHolder {
    PageList s;
    bool ok;
    ListHolder(zigstd::Allocator a, const Options &o) { ok = PageList::init(a, o, &s); }
    explicit ListHolder(const Options &o) { ok = PageList::init(talloc(), o, &s); }
    ~ListHolder() {
        if (ok) s.deinit();
    }
    PageList *operator->() { return &s; }
    PageList &operator*() { return s; }
};

static Options opts(size::CellCountInt cols, size::CellCountInt rows, Maybe<size_t> max_size = Maybe<size_t>(),
                    Maybe<size_t> max_lines = Maybe<size_t>()) {
    return Options(cols, rows, max_size, max_lines);
}

static uint32_t cpAt(Page *p, size_t x, size_t y) { return p->getRowAndCell(x, y).cell->codepoint(); }

/* ---- Builder / PageAllocation ---- */

TEST(page_list, PageList_Builder_transfers_mixed_width_pages) {
    /* Build two populated pages whose widths differ from each other and from
     * the final active-area width. The first page also includes one row of
     * incidental history above the three-row active area. */
    PageList result;
    {
        Builder builder;
        ASSERT_TRUE(Builder::init(talloc(), opts(4, 3), &builder));

        Page *first = builder.allocatePage(Capacity(2, 2));
        first->size.rows = 2;
        *first->getRowAndCell(0, 0).cell = page::Cell::init('A');

        Page *second = builder.allocatePage(Capacity(4, 2));
        second->size.rows = 2;
        *second->getRowAndCell(0, 0).cell = page::Cell::init('B');

        ASSERT_TRUE(builder.finish(&result) == Builder::FinishError::none);
        builder.deinit();
    }

    /* Successful finish transfers ownership and initializes the PageList's
     * desired geometry, viewport, and required tracked viewport pin. */
    ASSERT_TRUE(4 == result.cols);
    ASSERT_TRUE(3 == result.rows);
    ASSERT_TRUE(2 == result.totalPages());
    ASSERT_TRUE(1 == result.countTrackedPins());
    ASSERT_TRUE(Viewport::active == result.viewport);

    /* Complete pages and their contents are preserved in insertion order,
     * including widths which have not yet been reflowed. */
    const Pin screen_top = result.getTopLeft(point::Tag::screen);
    ASSERT_TRUE(2 == screen_top.node->cols());
    ASSERT_TRUE('A' == cpAt(screen_top.node->page(), 0, 0));

    /* The active area is calculated backward from the newest page, so it
     * begins at row one of the oldest page and leaves row zero as history. */
    const Pin active_top = result.getTopLeft(point::Tag::active);
    ASSERT_TRUE(screen_top.node == active_top.node);
    ASSERT_TRUE(1 == active_top.y);
    ASSERT_TRUE(4 == active_top.node->next->cols());
    ASSERT_TRUE('B' == cpAt(active_top.node->next->page(), 0, 0));

    result.assertIntegrity();
    result.deinit();
}

TEST(page_list, PageList_Builder_validates_the_finished_list) {
    PageList out;
    /* The desired PageList geometry must describe a non-empty screen. */
    {
        Builder builder;
        ASSERT_TRUE(Builder::init(talloc(), opts(0, 1), &builder));
        ASSERT_TRUE(builder.finish(&out) == Builder::FinishError::InvalidDimensions);
        builder.deinit();
    }

    /* A PageList cannot be finished without any backing pages. */
    {
        Builder builder;
        ASSERT_TRUE(Builder::init(talloc(), opts(1, 1), &builder));
        ASSERT_TRUE(builder.finish(&out) == Builder::FinishError::NoPages);
        builder.deinit();
    }

    /* Allocated capacity alone is insufficient: callers must populate a
     * nonzero logical page size before transferring ownership. */
    {
        Builder builder;
        ASSERT_TRUE(Builder::init(talloc(), opts(1, 1), &builder));
        Page *page = builder.allocatePage(Capacity(1, 1));
        page->size.rows = 0;
        ASSERT_TRUE(builder.finish(&out) == Builder::FinishError::InvalidPageDimensions);
        builder.deinit();
    }

    /* The populated pages must contain enough rows to cover the active area. */
    {
        Builder builder;
        ASSERT_TRUE(Builder::init(talloc(), opts(1, 2), &builder));
        Page *page = builder.allocatePage(Capacity(1, 1));
        page->size.rows = 1;
        ASSERT_TRUE(builder.finish(&out) == Builder::FinishError::InsufficientRows);
        builder.deinit();
    }
}

TEST(page_list, PageList_Builder_finish_is_transactional_on_allocation_failure) {
    /* Construct a valid builder so finish reaches its fallible bookkeeping
     * allocations after all page and geometry validation succeeds. */
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    Builder builder;
    ASSERT_TRUE(Builder::init(failing.allocator(), opts(1, 1), &builder));
    Page *page = builder.allocatePage(Capacity(1, 1));
    page->size.rows = 1;

    /* The pools are preheated, so the next general allocation is the tracked
     * viewport pin map created by finish. */
    failing.fail_index = failing.alloc_index;
    PageList out;
    ASSERT_TRUE(builder.finish(&out) == Builder::FinishError::OutOfMemory);
    ASSERT_TRUE(failing.has_induced_failure);

    /* Failed finish leaves page ownership with the builder so its normal
     * deinit path can release the still-linked page. */
    ASSERT_TRUE(builder.pages.first != nullptr);
    ASSERT_TRUE(builder.pages.first == builder.pages.last);
    builder.deinit();
}

TEST(page_list, PageList_PageAllocation_finalizes_pages_and_preserves_live_state) {
    /* Build an existing list with history, a two-row active area, an external
     * active pin, and a pinned viewport whose absolute offset is cached. */
    PageList result;
    {
        Builder builder;
        ASSERT_TRUE(Builder::init(talloc(), opts(4, 2), &builder));

        Page *first = builder.allocatePage(Capacity(3, 2));
        first->size.rows = 2;
        *first->getRowAndCell(0, 0).cell = page::Cell::init('C');

        Page *second = builder.allocatePage(Capacity(4, 2));
        second->size.rows = 2;
        *second->getRowAndCell(0, 0).cell = page::Cell::init('D');

        ASSERT_TRUE(builder.finish(&result) == Builder::FinishError::none);
        builder.deinit();
    }

    Node *old_first = result.pages.first;
    Node *old_last = result.pages.last;
    const Pin active_top = result.getTopLeft(point::Tag::active);
    Pin *tracked_active = result.trackPin(active_top);
    result.scroll(PageList::Scroll::rowAt(1));
    ASSERT_TRUE(Viewport::pin == result.viewport);
    ASSERT_TRUE(1 == result.scrollbar().offset);

    /* Prepend differently sized historical pages newest-first. Each page is
     * populated while detached and joins the live list only on success. */
    {
        PageAllocation allocation;
        ASSERT_TRUE(result.allocatePage(Capacity(4, 1), &allocation));
        Page *page = allocation.page();
        page->size.rows = 1;
        *page->getRowAndCell(0, 0).cell = page::Cell::init('B');
        ASSERT_TRUE(allocation.finalize(PageAllocation::Location::prepend) == PageAllocation::FinalizeError::none);
        allocation.deinit();
    }
    {
        PageAllocation allocation;
        ASSERT_TRUE(result.allocatePage(Capacity(2, 2), &allocation));
        Page *page = allocation.page();
        page->size.rows = 2;
        *page->getRowAndCell(0, 0).cell = page::Cell::init('A');
        ASSERT_TRUE(allocation.finalize(PageAllocation::Location::prepend) == PageAllocation::FinalizeError::none);
        allocation.deinit();
    }

    /* Repeated prepends reconstruct oldest-to-newest order without replacing
     * any existing nodes or tracked pins. */
    ASSERT_TRUE(4 == result.totalPages());
    ASSERT_TRUE(7 == result.total_rows);
    ASSERT_TRUE(old_last == result.pages.last);
    ASSERT_TRUE(old_first == result.pages.first->next->next);
    ASSERT_TRUE('A' == cpAt(result.pages.first->page(), 0, 0));
    ASSERT_TRUE('B' == cpAt(result.pages.first->next->page(), 0, 0));
    ASSERT_TRUE(active_top.eql(result.getTopLeft(point::Tag::active)));
    ASSERT_TRUE(active_top.eql(*tracked_active));

    /* The viewport remains pinned to the same content, while its cached row
     * offset and the scrollbar total include the three new historical rows. */
    ASSERT_TRUE(Viewport::pin == result.viewport);
    ASSERT_TRUE(old_first == result.viewport_pin->node);
    ASSERT_TRUE(1 == result.viewport_pin->y);
    const PageList::Scrollbar scrollbar_state = result.scrollbar();
    ASSERT_TRUE(7 == scrollbar_state.total);
    ASSERT_TRUE(4 == scrollbar_state.offset);
    ASSERT_TRUE(2 == scrollbar_state.len);

    result.assertIntegrity();
    result.deinit();
}

TEST(page_list, PageList_PageAllocation_stays_detached_until_finalize) {
    ListHolder result(opts(1, 1));

    Node *initial_first = result->pages.first;
    Node *initial_last = result->pages.last;
    const size_t initial_total_rows = result->total_rows;
    const size_t initial_page_size = result->page_size;

    /* Allocating and populating a detached page does not alter any live list
     * links or accounting. Deinit returns it to the same PageList pools. */
    PageAllocation detached;
    ASSERT_TRUE(result->allocatePage(Capacity(1, 1), &detached));
    detached.page()->size.rows = 1;
    ASSERT_TRUE(initial_first == result->pages.first);
    ASSERT_TRUE(initial_last == result->pages.last);
    ASSERT_TRUE(initial_total_rows == result->total_rows);
    ASSERT_TRUE(initial_page_size == result->page_size);
    result->assertIntegrity();
    detached.deinit();
    result->assertIntegrity();

    /* Invalid populated dimensions leave ownership with the allocation so it
     * can still be released normally. */
    PageAllocation invalid;
    ASSERT_TRUE(result->allocatePage(Capacity(1, 1), &invalid));
    ASSERT_TRUE(invalid.finalize(PageAllocation::Location::prepend) ==
                PageAllocation::FinalizeError::InvalidPageDimensions);

    ASSERT_TRUE(initial_first == result->pages.first);
    ASSERT_TRUE(initial_last == result->pages.last);
    ASSERT_TRUE(initial_total_rows == result->total_rows);
    ASSERT_TRUE(initial_page_size == result->page_size);
    result->assertIntegrity();
    invalid.deinit();
}

TEST(page_list, PageList_PageAllocation_rejects_limits_before_modifying_the_destination) {
    ListHolder result(opts(1, 1, (size_t)0));

    /* The effective minimum permits one complete page beyond the active page.
     * Fill that allowance so the following allocation exceeds the byte limit. */
    {
        PageAllocation allocation;
        ASSERT_TRUE(result->allocatePage(Capacity(1, 1), &allocation));
        allocation.page()->size.rows = 1;
        ASSERT_TRUE(allocation.finalize(PageAllocation::Location::prepend) == PageAllocation::FinalizeError::none);
        allocation.deinit();
    }

    Node *before_first = result->pages.first;
    const size_t before_total_rows = result->total_rows;
    const size_t before_page_size = result->page_size;

    PageAllocation allocation;
    ASSERT_TRUE(result->allocatePage(Capacity(1, 1), &allocation));
    allocation.page()->size.rows = 1;
    ASSERT_TRUE(allocation.finalize(PageAllocation::Location::prepend) ==
                PageAllocation::FinalizeError::MaxSizeExceeded);

    ASSERT_TRUE(before_first == result->pages.first);
    ASSERT_TRUE(before_total_rows == result->total_rows);
    ASSERT_TRUE(before_page_size == result->page_size);
    result->assertIntegrity();
    allocation.deinit();
}

TEST(page_list, PageList_PageAllocation_allocation_failure_leaves_list_unchanged) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    ListHolder result(failing.allocator(), opts(1, 1));

    Node *initial_first = result->pages.first;
    const size_t initial_total_rows = result->total_rows;
    const size_t initial_page_size = result->page_size;

    /* Existing pool capacity is deliberately an implementation detail. Allow
     * preheated slots to succeed until allocation reaches node-pool growth. */
    failing.fail_index = failing.alloc_index;
    PageAllocation allocations[64];
    size_t allocation_count = 0;

    bool failed = false;
    for (size_t i = 0; i < 64; i++) {
        if (!result->allocatePage(Capacity(1, 1), &allocations[allocation_count])) {
            failed = true;
            break;
        }
        allocation_count += 1;
    }
    ASSERT_TRUE(failed);
    ASSERT_TRUE(failing.has_induced_failure);

    /* Detached allocations and failed pool growth never publish into the live
     * list; the deferred cleanup returns every successful allocation. */
    ASSERT_TRUE(initial_first == result->pages.first);
    ASSERT_TRUE(initial_total_rows == result->total_rows);
    ASSERT_TRUE(initial_page_size == result->page_size);
    result->assertIntegrity();
    for (size_t i = 0; i < allocation_count; i++) allocations[i].deinit();
}

/* ---- test helpers (upstream's *ForTest functions) ---- */

typedef PageList::IncrementalCompressionState CState;
typedef PageList::IncrementalCompressionResult CResult;
typedef PageList::CompressMode CMode;

static bool mixedWidthPinListForTest(PageList *result) {
    if (!PageList::init(talloc(), opts(2, 1), result)) return false;

    /* This deliberately constructs a layout that normal PageList operations
     * do not expose yet. Keep integrity checks paused through deinit so the
     * fixture can exercise mixed-width traversal in isolation. */
    result->pauseIntegrityChecks(true);

    const size::CellCountInt widths[] = {4, 3};
    for (size::CellCountInt c : widths) {
        Node *node = result->createPage(PageList::CreatePage(Capacity(c, 1)));
        if (!node) return false;
        node->page()->size.rows = 1;
        result->pages.append(node);
        result->total_rows += 1;
    }

    /* Desired geometry is wider than the first and last stored pages. */
    result->cols = 4;
    return true;
}

/* Grow a test PageList until it contains at least `count` complete history
 * pages. The production cold-page boundary is intentionally reused here so
 * tests do not duplicate the row-to-page arithmetic. */
static bool growColdPagesForTest(PageList *self, size_t count) {
    for (;;) {
        Node *active_node = self->getTopLeft(point::Tag::active).node;
        size_t cold_count = 0;
        for (Node *node = self->pages.first; node; node = node->next) {
            if (node == active_node) break;
            cold_count += 1;
        }

        if (cold_count >= count) return true;
        Node *g;
        if (!self->grow(&g)) return false;
    }
}

/* Fill the current tail page to capacity without allocating a successor.
 * Capturing the tail before the loop makes this stop at the allocation
 * boundary needed by bounded-pruning tests. */
static bool fillLastPageForTest(PageList *self) {
    Node *last = self->pages.last;
    while (last->rows() < last->capacity().rows) {
        Node *g;
        if (!self->grow(&g)) return false;
    }
    return true;
}

/* Verify every live page belongs to the current validity epoch, has an
 * allocated generation below the next serial, and validates through the same
 * pointer-plus-generation lookup used by external references. */
static bool expectLivePageSerialsValidForTest(const PageList *self) {
    for (const Node *live = self->pages.first; live; live = live->next) {
        if (!(live->serial >= self->page_serial_epoch)) return false;
        if (!(live->serial < self->page_serial)) return false;
        if (!self->nodeIsValid(live, live->serial)) return false;
    }
    return true;
}

static Node *growNode(PageList *s) {
    Node *g = nullptr;
    const bool ok = s->grow(&g);
    assert(ok);
    (void)ok;
    return g;
}

static bool stateEq(const CState &a, const CState &b) {
    return a.flags.did_compress == b.flags.did_compress && a.flags.verifying == b.flags.verifying &&
           a.activity_serial == b.activity_serial && a.last_serial.has == b.last_serial.has &&
           (!a.last_serial.has || a.last_serial.value == b.last_serial.value) && a.next_serial == b.next_serial;
}

static CState mkState(bool did, bool ver, uint64_t act, Maybe<uint64_t> last, uint64_t next) {
    CState s;
    s.flags.did_compress = did;
    s.flags.verifying = ver;
    s.activity_serial = act;
    s.last_serial = last;
    s.next_serial = next;
    return s;
}

static bool pointEq(const Maybe<Point> &a, const Point &b) { return a.has && a.value.eql(b); }

/* ---- Pin movement ---- */

TEST(page_list, PageList_Pin_row_movement_clamps_across_mixed_width_pages) {
    PageList s;
    ASSERT_TRUE(mixedWidthPinListForTest(&s));

    Node *first = s.pages.first;
    Node *second = first->next;
    Node *third = second->next;

    ASSERT_TRUE(Pin::at(third, 2, 0).eql(Pin::at(second, 3, 0).down(1).value));
    ASSERT_TRUE(Pin::at(first, 1, 0).eql(Pin::at(second, 3, 0).up(1).value));

    {
        const Pin::Overflow o = Pin::at(second, 3, 0).downOverflow(10);
        ASSERT_TRUE(o.tag == Pin::Overflow::Tag::overflow);
        ASSERT_TRUE(Pin::at(third, 2, 0).eql(o.overflow.end));
    }
    {
        const Pin::Overflow o = Pin::at(second, 3, 0).upOverflow(10);
        ASSERT_TRUE(o.tag == Pin::Overflow::Tag::overflow);
        ASSERT_TRUE(Pin::at(first, 1, 0).eql(o.overflow.end));
    }
    s.deinit();
}

TEST(page_list, PageList_Pin_wrapping_crosses_mixed_width_pages) {
    PageList s;
    ASSERT_TRUE(mixedWidthPinListForTest(&s));

    Node *first = s.pages.first;
    Node *second = first->next;
    Node *third = second->next;

    ASSERT_TRUE(Pin::at(third, 2, 0).eql(Pin::at(first, 1, 0).rightWrap(7).value));
    ASSERT_TRUE(Pin::at(second, 0, 0).eql(Pin::at(third, 2, 0).leftWrap(6).value));
    ASSERT_FALSE(Pin(first).leftWrap(1).has);
    ASSERT_FALSE(Pin::at(third, 2, 0).rightWrap(1).has);
    s.deinit();
}

TEST(page_list, PageList_Pin_rejects_columns_beyond_mixed_width_page_bounds) {
    PageList s;
    ASSERT_TRUE(mixedWidthPinListForTest(&s));

    ASSERT_TRUE(s.pin(Point::screen(1, 0)).has);
    ASSERT_FALSE(s.pin(Point::screen(2, 0)).has);
    ASSERT_TRUE(s.pin(Point::screen(3, 1)).has);
    ASSERT_FALSE(s.pin(Point::screen(3, 2)).has);
    s.deinit();
}

TEST(page_list, PageList_Pin_rightWrap_exact_row_multiple) {
    ListHolder s(opts(10, 3));

    const Pin start = s->pin(Point::active(5, 0)).value;
    const Pin wrapped = start.rightWrap(14).value;
    (void)wrapped.rowAndCell();

    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::active, wrapped), Point::active(9, 1)));
}

TEST(page_list, PageList_Pin_leftWrap_exact_row_multiple) {
    ListHolder s(opts(10, 3));

    const Pin start = s->pin(Point::active(5, 2)).value;
    const Pin wrapped = start.leftWrap(15).value;
    (void)wrapped.rowAndCell();

    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::active, wrapped), Point::active(0, 1)));
}

TEST(page_list, PageList_Pin_rightWrap_maximum_distance) {
    ListHolder s(opts(1, 3));

    const Pin start = s->pin(Point::active(0, 0)).value;
    ASSERT_FALSE(start.rightWrap(SIZE_MAX).has);
}

TEST(page_list, PageList_Pin_leftWrap_maximum_distance) {
    ListHolder s(opts(1, 3));

    const Pin start = s->pin(Point::active(0, 2)).value;
    ASSERT_FALSE(start.leftWrap(SIZE_MAX).has);
}

/* ---- Compression ---- */

TEST(page_list, PageList_incremental_compression_skips_visible_history) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 3));

    const uint64_t initial_activity = s->page_compression.activity_serial;
    ASSERT_TRUE(initial_activity > 0);

    s->scroll(PageList::Scroll::top());
    const uint64_t top_activity = s->page_compression.activity_serial;
    ASSERT_TRUE(top_activity != initial_activity);
    ASSERT_TRUE(CResult::complete == s->compress(CMode::drain));

    Node *first = s->pages.first;
    Node *second = first->next;
    ASSERT_TRUE(first == s->getTopLeft(point::Tag::viewport).node);
    ASSERT_TRUE(first == s->getBottomRight(point::Tag::viewport).value.node);
    ASSERT_FALSE(first->isCompressed());

    PageList::CompressionIterator eligible = PageList::CompressionIterator::init(&*s);
    size_t compressed_pages = 0;
    while (Node *node = eligible.next()) {
        compressed_pages += 1;
        ASSERT_TRUE(node->isCompressed());
    }
    ASSERT_TRUE(compressed_pages > 0);

    /* Move the viewport to the start of the second page. Rendering it restores
     * that page, while the first page which just left view becomes eligible. */
    s->scroll(PageList::Scroll::rowAt(first->rows()));
    ASSERT_TRUE(second == s->getTopLeft(point::Tag::viewport).node);
    (void)second->page();
    ASSERT_FALSE(second->isCompressed());
    (void)s->compress(CMode::drain);
    ASSERT_TRUE(first->isCompressed());
    ASSERT_FALSE(second->isCompressed());

    /* Returning to the active area makes every complete historical page
     * eligible again, including the page which was just visible. */
    s->scroll(PageList::Scroll::active());
    (void)s->compress(CMode::drain);
    ASSERT_TRUE(second->isCompressed());
    ASSERT_FALSE(s->page_compression.flags.did_compress);
    ASSERT_FALSE(s->page_compression.flags.verifying);
    ASSERT_FALSE(s->page_compression.last_serial.has);
    ASSERT_TRUE(0 == s->page_compression.next_serial);
}

TEST(page_list, PageList_owns_incremental_compression_state) {
    ListHolder s(opts(80, 24));

    const CState state = mkState(true, true, 42, (uint64_t)42, 43);
    const CState only43 = mkState(false, false, 43, Maybe<uint64_t>(), 0);
    const CState only42 = mkState(false, false, 42, Maybe<uint64_t>(), 0);

    s->page_compression = state;
    s->scroll(PageList::Scroll::top());
    ASSERT_TRUE(stateEq(only43, s->page_compression));

    /* Every scroll restarts traversal, even if clamping leaves the viewport in
     * the same place. Missing an eligible page is worse than a no-op pass. */
    s->page_compression = state;
    s->scroll(PageList::Scroll::top());
    ASSERT_TRUE(stateEq(only43, s->page_compression));

    s->page_compression = state;
    s->scroll(PageList::Scroll::active());
    ASSERT_TRUE(stateEq(only43, s->page_compression));

    s->page_compression = state;
    {
        PageList::Resize r;
        r.cols = (size::CellCountInt)80;
        r.rows = (size::CellCountInt)24;
        ASSERT_TRUE(s->resize(r));
    }
    ASSERT_TRUE(stateEq(only43, s->page_compression));

    s->page_compression = state;
    s->reset();
    ASSERT_TRUE(stateEq(only42, s->page_compression));

    s->page_compression = state;
    ASSERT_TRUE(CResult::complete == s->compress(CMode::full));
    ASSERT_TRUE(stateEq(only42, s->page_compression));

    s->page_compression = state;
    PageList cloned;
    ASSERT_TRUE(s->clone(talloc(), PageList::Clone(Point::active()), &cloned) == page::PageError::none);
    ASSERT_TRUE(stateEq(CState(), cloned.page_compression));
    cloned.deinit();
}

TEST(page_list, PageList_replacements_preserve_compression_continuation_and_mark_activity) {
    ListHolder s(opts(80, 24));

    const CState state = mkState(false, true, 42, (uint64_t)7, 8);
    const CState expected = mkState(false, true, 43, (uint64_t)7, 8);

    s->page_compression = state;
    Node *replacement;
    ASSERT_TRUE(s->increaseCapacity(s->pages.first, Maybe<PageList::IncreaseCapacity>(), &replacement) ==
                PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(stateEq(expected, s->page_compression));

    s->page_compression = state;
    Node *compacted;
    ASSERT_TRUE(s->compact(replacement, &compacted));
    ASSERT_TRUE(compacted != nullptr);
    ASSERT_TRUE(stateEq(expected, s->page_compression));
}

TEST(page_list, PageList_incremental_compression_bounds_inspected_pages) {
    const size_t max_inspected = PageList::incremental_compression_max_inspected;
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, max_inspected + 1));

    /* Precompress every candidate so the incremental pass exercises its
     * metadata-only skip budget without stopping at a resident attempt. */
    (void)s->compress(CMode::full);
    s->page_compression.reset();
    s->page_compression.markActivity();
    ASSERT_TRUE(max_inspected + 1 == s->memoryStats().compressed_pages);

    Node *expected_last = s->pages.first;
    for (size_t i = 1; i < max_inspected; i++) expected_last = expected_last->next;

    const CResult first = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == first);
    ASSERT_TRUE(expected_last->serial == s->page_compression.last_serial.value);

    const CResult second = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == second);
    ASSERT_TRUE(s->page_compression.flags.verifying);
    ASSERT_FALSE(s->page_compression.last_serial.has);

    /* The verification pass is bounded independently, too. */
    ASSERT_TRUE(CResult::pending == s->compress(CMode::incremental));
    ASSERT_TRUE(CResult::complete == s->compress(CMode::incremental));
}

TEST(page_list, PageList_incremental_compression_advances_after_failure) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 2));

    Node *first = s->pages.first;
    Node *second = first->next;
    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(0x494E435250415353ull);
    zigstd::Random(&prng).bytes(first->page()->memory, first->page()->memory_len);

    const CResult failed = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == failed);
    ASSERT_FALSE(first->isCompressed());

    /* The unsuccessful first page does not stall the pass. The next step
     * continues at the following serial and compresses that page. */
    const CResult continued = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == continued);
    ASSERT_TRUE(second->isCompressed());

    /* Wisp: the random bytes are not a valid page; zero them so deinit's
     * integrity check sees a well-formed (empty) page again. */
    first->page()->reinit();
    first->page()->size.rows = first->capacity().rows;
}

TEST(page_list, PageList_incremental_compression_advances_after_allocation_failure) {
    zigstd::FailingAllocator failing(talloc(), SIZE_MAX);
    const zigstd::Allocator alloc = failing.allocator();
    ListHolder s(alloc, opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 2));
    Node *first = s->pages.first;
    Node *second = first->next;

    /* Pool preheating supplies compression scratch. Failing the allocator's
     * next request therefore rejects the exact encoded allocation while the
     * source page and pass remain valid. */
    failing.fail_index = failing.alloc_index;
    const CResult failed = s->compress(CMode::incremental);
    ASSERT_TRUE(failing.has_induced_failure);
    ASSERT_TRUE(CResult::pending == failed);
    ASSERT_FALSE(first->isCompressed());

    /* Allow allocations again. The pass must continue with the following page
     * rather than retrying the failed candidate. */
    failing.fail_index = SIZE_MAX;
    const CResult continued = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == continued);
    ASSERT_TRUE(second->isCompressed());
}

TEST(page_list, PageList_incremental_compression_advances_after_decommit_failure) {
    typedef PageList::compressPage_tw tw;

    {
        ListHolder s(opts(80, 24));
        ASSERT_TRUE(growColdPagesForTest(&*s, 2));

        tw::errorAlways(PageList::CompressPageTw::decommit, PageList::CompressPageTwError::DecommitFailed);
        const CResult failed = s->compress(CMode::incremental);
        ASSERT_TRUE(CResult::pending == failed);
        ASSERT_FALSE(s->pages.first->isCompressed());
        ASSERT_TRUE(tw::end(tripwire::ResetMode::reset));

        /* The failed candidate remains resident and the pass continues at the
         * next serial once reclamation is available again. */
        const CResult continued = s->compress(CMode::incremental);
        ASSERT_TRUE(CResult::pending == continued);
        ASSERT_TRUE(s->pages.first->next->isCompressed());
    }
    (void)tw::end(tripwire::ResetMode::reset);
}

TEST(page_list, PageList_incremental_compression_restarts_after_replacement) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));

    const CResult initial = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == initial);
    ASSERT_TRUE(s->pages.first->isCompressed());

    Node *old = s->pages.first;
    const uint64_t old_serial = old->serial;
    Node *replacement = old;
    while (replacement->page()->memory_len <= PageList::std_size) {
        ASSERT_TRUE(s->increaseCapacity(replacement, PageList::IncreaseCapacity::grapheme_bytes, &replacement) ==
                    PageList::IncreaseCapacityError::none);
    }
    ASSERT_TRUE(replacement->serial != old_serial);
    ASSERT_TRUE(replacement->page()->memory_len > PageList::std_size);
    ASSERT_FALSE(replacement->isCompressed());

    /* The exact continuation serial disappeared with the old node. The pass
     * restarts at the first page and considers the oversized replacement. */
    const CResult restarted = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == restarted);
    ASSERT_TRUE(replacement->isCompressed());
}

TEST(page_list, PageList_incremental_compression_restarts_after_reset) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));

    const CResult initial = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == initial);
    ASSERT_TRUE(s->pages.first->isCompressed());

    /* Reset replaces every page and clears the PageList-owned traversal. */
    s->reset();
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));
    const CResult restarted = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == restarted);
    ASSERT_TRUE(s->pages.first->isCompressed());
}

TEST(page_list, PageList_incremental_compression_restarts_after_active_boundary_resize) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));

    const CResult initial = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == initial);
    ASSERT_TRUE(s->pages.first->isCompressed());

    Node *first = s->pages.first;
    const size::CellCountInt all_rows = (size::CellCountInt)s->total_rows;
    {
        PageList::Resize r;
        r.rows = all_rows;
        ASSERT_TRUE(s->resize(r));
    }
    ASSERT_TRUE(first == s->getTopLeft(point::Tag::active).node);

    /* Restore the page while it is active. Resize reset the traversal, and
     * active contents remain ineligible. */
    (void)first->page();
    const CResult active = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == active);
    ASSERT_TRUE(CResult::complete == s->compress(CMode::incremental));

    /* Shrinking the active area makes the page fully historical again. The
     * resize reset the PageList-owned cursor, so the next step can reclaim it. */
    {
        PageList::Resize r;
        r.rows = (size::CellCountInt)24;
        ASSERT_TRUE(s->resize(r));
    }
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));
    ASSERT_TRUE(CResult::pending == s->compress(CMode::incremental));
    ASSERT_TRUE(first->isCompressed());
}

TEST(page_list, PageList_incremental_compression_restarts_after_prune_reuse) {
    ListHolder s(opts(80, 24, 2 * PageList::PagePool::item_size));
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));

    const CResult initial = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == initial);
    ASSERT_TRUE(s->pages.first->isCompressed());

    Node *reused = s->pages.first;
    const uint64_t old_serial = reused->serial;
    while (s->pages.last->rows() < s->pages.last->capacity().rows) {
        (void)growNode(&*s);
    }
    ASSERT_TRUE(reused == growNode(&*s));
    ASSERT_TRUE(reused->serial != old_serial);

    /* Make the remaining old page fully historical. The continuation serial
     * disappeared when its node was recycled, so the pass safely restarts. */
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));
    (void)s->compress(CMode::incremental);
    ASSERT_TRUE(1 == s->memoryStats().compressed_pages);
}

TEST(page_list, PageList_bounded_pruning_after_partial_erase_preserves_live_serials) {
    ListHolder s(opts(80, 24, 2 * PageList::PagePool::item_size));

    while (s->totalPages() < 2) (void)growNode(&*s);
    Node *first = s->pages.first;
    const uint64_t old_serial = first->serial;
    const size_t old_rows = first->rows();

    s->eraseHistory(Point::history(0, 0));
    ASSERT_TRUE(first == s->pages.first);
    ASSERT_TRUE(old_rows - 1 == first->rows());
    ASSERT_FALSE(s->nodeIsValid(first, old_serial));

    ASSERT_TRUE(fillLastPageForTest(&*s));
    (void)growNode(&*s);
    ASSERT_TRUE(expectLivePageSerialsValidForTest(&*s));
}

TEST(page_list, PageList_partial_erase_restarts_compression_before_continuation) {
    const size_t max_inspected = PageList::incremental_compression_max_inspected;
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, max_inspected + 1));
    (void)s->compress(CMode::full);

    Node *first = s->pages.first;
    ASSERT_TRUE(first->isCompressed());

    Node *marker = first;
    for (size_t i = 1; i < max_inspected; i++) marker = marker->next;
    s->page_compression = mkState(false, true, 0, marker->serial, s->page_serial);

    const uint64_t activity = s->page_compression.activity_serial;
    s->eraseHistory(Point::history(0, 0));
    ASSERT_FALSE(first->isCompressed());
    ASSERT_TRUE(activity != s->page_compression.activity_serial);

    /* The changed generation is before the saved marker, so continuation must
     * restart and recompress it instead of reporting verification complete. */
    ASSERT_TRUE(CResult::pending == s->compress(CMode::incremental));
    ASSERT_TRUE(first->isCompressed());
}

TEST(page_list, PageList_bounded_pruning_after_split_invalidation_preserves_live_serials) {
    ListHolder s(opts(80, 24, 2 * PageList::PagePool::item_size));

    while (s->totalPages() < 2) (void)growNode(&*s);
    Node *first = s->pages.first;
    const uint64_t old_serial = first->serial;
    const uint64_t activity = s->page_compression.activity_serial;

    ASSERT_TRUE(s->split(Pin(first, (size::CellCountInt)(first->rows() / 2), 0)) == PageList::SplitError::none);
    ASSERT_FALSE(s->nodeIsValid(first, old_serial));
    ASSERT_TRUE(activity != s->page_compression.activity_serial);

    ASSERT_TRUE(fillLastPageForTest(&*s));
    (void)growNode(&*s);
    ASSERT_TRUE(expectLivePageSerialsValidForTest(&*s));
}

TEST(page_list, PageList_repeated_bounded_pruning_after_split_preserves_live_serials) {
    ListHolder s(opts(80, 24, 3 * PageList::PagePool::item_size));

    const uint64_t epoch = s->page_serial_epoch;
    while (s->totalPages() < 3) (void)growNode(&*s);
    Node *first = s->pages.first;
    ASSERT_TRUE(s->split(Pin(first, (size::CellCountInt)(first->rows() / 2), 0)) == PageList::SplitError::none);

    /* The split target has a fresh serial but precedes older successor pages.
     * Prune both the old source and then that target while verifying ordinary
     * list mutation does not advance the whole-list validity epoch. */
    for (int k = 0; k < 2; k++) {
        while (s->pages.last->rows() < s->pages.last->capacity().rows) {
            (void)growNode(&*s);
        }
        (void)growNode(&*s);

        /* Ordinary pruning invalidates one generation at a time through live
         * list validation; only reset may begin a new whole-list epoch. */
        ASSERT_TRUE(epoch == s->page_serial_epoch);
        ASSERT_TRUE(expectLivePageSerialsValidForTest(&*s));
    }
}

TEST(page_list, PageList_bounded_pruning_after_front_replacement_preserves_live_serials) {
    ListHolder s(opts(80, 24, 2 * PageList::PagePool::item_size));

    while (s->totalPages() < 2) (void)growNode(&*s);
    Node *old = s->pages.first;
    const uint64_t old_serial = old->serial;
    Node *replacement;
    ASSERT_TRUE(s->increaseCapacity(old, Maybe<PageList::IncreaseCapacity>(), &replacement) ==
                PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(replacement != old);
    ASSERT_FALSE(s->nodeIsValid(old, old_serial));

    ASSERT_TRUE(fillLastPageForTest(&*s));
    (void)growNode(&*s);

    ASSERT_TRUE(expectLivePageSerialsValidForTest(&*s));
}

static bool statsEq(const PageList::MemoryStats &a, const PageList::MemoryStats &b) {
    return a.resident_pages == b.resident_pages && a.compressed_pages == b.compressed_pages &&
           a.raw_bytes == b.raw_bytes && a.resident_raw_bytes == b.resident_raw_bytes &&
           a.decommitted_raw_bytes == b.decommitted_raw_bytes &&
           a.resident_backing_bytes == b.resident_backing_bytes && a.encoded_bytes == b.encoded_bytes;
}

static uint8_t *dupeMem(const Page *p) {
    uint8_t *d = (uint8_t *)malloc(p->memory_len);
    memcpy(d, p->memory, p->memory_len);
    return d;
}

static bool allZero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

TEST(page_list, PageList_bounded_pruning_after_middle_replacement_preserves_live_serials) {
    ListHolder s(opts(80, 24, 3 * PageList::PagePool::item_size));

    while (s->totalPages() < 3) (void)growNode(&*s);
    Node *old = s->pages.first->next;
    const uint64_t old_serial = old->serial;
    Node *replacement;
    ASSERT_TRUE(s->increaseCapacity(old, Maybe<PageList::IncreaseCapacity>(), &replacement) ==
                PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(replacement != old);
    ASSERT_FALSE(s->nodeIsValid(old, old_serial));

    /* Prune the original first page and then the fresh middle replacement. */
    for (int k = 0; k < 2; k++) {
        ASSERT_TRUE(fillLastPageForTest(&*s));
        (void)growNode(&*s);
        ASSERT_TRUE(expectLivePageSerialsValidForTest(&*s));
    }
}

TEST(page_list, PageList_incremental_compression_restarts_after_earlier_replacement) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 3));

    (void)s->compress(CMode::incremental);
    (void)s->compress(CMode::incremental);
    ASSERT_TRUE(s->pages.first->isCompressed());
    ASSERT_TRUE(s->pages.first->next->isCompressed());

    /* Replace a page before the still-valid continuation marker. The list's
     * allocation serial changes even though the marker itself remains, so the
     * next step must restart and inspect the replacement. */
    Node *old_first = s->pages.first;
    const uint64_t old_serial = old_first->serial;
    Node *replacement;
    ASSERT_TRUE(s->increaseCapacity(old_first, PageList::IncreaseCapacity::grapheme_bytes, &replacement) ==
                PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(replacement->serial != old_serial);
    ASSERT_FALSE(replacement->isCompressed());

    (void)s->compress(CMode::incremental);
    ASSERT_TRUE(replacement->isCompressed());
}

TEST(page_list, PageList_incremental_compression_keeps_progress_after_tail_growth) {
    const size_t max_inspected = PageList::incremental_compression_max_inspected;
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, max_inspected + 1));
    (void)s->compress(CMode::full);
    s->page_compression.reset();
    s->page_compression.markActivity();

    Node *expected_last = s->pages.first;
    for (size_t i = 1; i < max_inspected; i++) expected_last = expected_last->next;

    const CResult first = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == first);
    ASSERT_TRUE(expected_last->serial == s->page_compression.last_serial.value);

    /* Allocate a new page at the active tail between steps. It is after the
     * continuation marker and must not restart progress through cold history. */
    const uint64_t next_serial = s->page_serial;
    while (s->page_serial == next_serial) (void)growNode(&*s);
    const CResult continued = s->compress(CMode::incremental);
    ASSERT_TRUE(CResult::pending == continued);
    ASSERT_TRUE(s->page_compression.flags.verifying);
}

TEST(page_list, PageList_memory_stats_do_not_restore_compressed_pages) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 2));

    const PageList::MemoryStats before = s->memoryStats();
    ASSERT_TRUE(s->totalPages() == before.resident_pages);
    ASSERT_TRUE(0 == before.compressed_pages);
    ASSERT_TRUE(s->page_size == before.raw_bytes);
    ASSERT_TRUE(before.raw_bytes == before.resident_raw_bytes);
    ASSERT_TRUE(0 == before.decommitted_raw_bytes);
    ASSERT_TRUE(s->page_size == before.resident_backing_bytes);
    ASSERT_TRUE(0 == before.encoded_bytes);
    ASSERT_TRUE(before.resident_backing_bytes == before.estimatedResidentBytes());
    ASSERT_TRUE(0 == before.estimatedSavings());

    (void)s->compress(CMode::full);
    Node *first = s->pages.first;
    ASSERT_TRUE(Node::Storage::compressed == first->storage());
    ASSERT_TRUE(first->pageIfResident() == nullptr);

    const PageList::MemoryStats after = s->memoryStats();
    ASSERT_TRUE(first->isCompressed());
    ASSERT_TRUE(s->totalPages() == after.resident_pages + after.compressed_pages);
    ASSERT_TRUE(2 == after.compressed_pages);
    ASSERT_TRUE(s->page_size == after.raw_bytes);
    ASSERT_TRUE(after.raw_bytes == after.resident_raw_bytes + after.decommitted_raw_bytes);
    ASSERT_TRUE(after.resident_backing_bytes + after.encoded_bytes == after.estimatedResidentBytes());
    ASSERT_TRUE(after.decommitted_raw_bytes - after.encoded_bytes == after.estimatedSavings());

    const size_t first_raw_len = first->metadata()->memory_len;
    const size_t first_encoded_len = first->data.compressed.encoded_len;
    (void)first->page();
    ASSERT_TRUE(Node::Storage::resident == first->storage());
    ASSERT_TRUE(first->pageIfResident() != nullptr);

    const PageList::MemoryStats restored = s->memoryStats();
    ASSERT_TRUE(after.resident_pages + 1 == restored.resident_pages);
    ASSERT_TRUE(after.compressed_pages - 1 == restored.compressed_pages);
    ASSERT_TRUE(after.raw_bytes == restored.raw_bytes);
    ASSERT_TRUE(after.resident_raw_bytes + first_raw_len == restored.resident_raw_bytes);
    ASSERT_TRUE(after.decommitted_raw_bytes - first_raw_len == restored.decommitted_raw_bytes);
    ASSERT_TRUE(after.resident_backing_bytes + first_raw_len == restored.resident_backing_bytes);
    ASSERT_TRUE(after.encoded_bytes - first_encoded_len == restored.encoded_bytes);
}

TEST(page_list, PageList_preserved_page_keeps_compressed_storage) {
    const zigstd::Allocator alloc = talloc();
    ListHolder s(alloc, opts(80, 24));

    Node *node = s->pages.first;
    Page *resident = node->page();
    resident->dirty = true;
    *resident->getRowAndCell(3, 2).cell = page::Cell::init('X');

    /* Resident nodes can be borrowed without allocating an unnecessary copy. */
    {
        zigstd::FailingAllocator failing(alloc, 0);
        Node::PreservedPage preserved;
        ASSERT_TRUE(node->pagePreservingState(failing.allocator(), &preserved));
        ASSERT_TRUE(preserved.tag == Node::PreservedPage::Tag::borrowed);
        ASSERT_TRUE(resident == preserved.borrowed);
        ASSERT_FALSE(failing.has_induced_failure);
        preserved.deinit();
    }

    uint8_t *expected = dupeMem(resident);
    uint8_t *const retained_ptr = resident->memory;
    const size_t mem_len = resident->memory_len;

    ASSERT_TRUE(s->compressPage(node));
    const PageList::MemoryStats stats = s->memoryStats();
    const size_t enc_len = node->data.compressed.encoded_len;
    uint8_t *expected_encoded = (uint8_t *)malloc(enc_len);
    memcpy(expected_encoded, node->data.compressed.encoded, enc_len);

    /* Test decommit simulates physical reclamation by clearing the retained
     * mapping. A preserved page must decode elsewhere rather than restoring
     * it. */
    ASSERT_TRUE(allZero(node->metadata()->memory, mem_len));

    /* Preserved-page allocation is opportunistic for callers. Failure leaves
     * the node and its compressed representation untouched. */
    {
        zigstd::FailingAllocator failing(alloc, 0);
        Node::PreservedPage p;
        ASSERT_FALSE(node->pagePreservingState(failing.allocator(), &p));
    }
    ASSERT_TRUE(Node::Storage::compressed == node->storage());
    ASSERT_TRUE(statsEq(stats, s->memoryStats()));

    Node::PreservedPage preserved;
    ASSERT_TRUE(node->pagePreservingState(alloc, &preserved));
    ASSERT_TRUE(preserved.tag == Node::PreservedPage::Tag::owned);
    const Page *page_ = preserved.page();

    ASSERT_TRUE(page_->memory != retained_ptr);
    ASSERT_TRUE(memcmp(expected, page_->memory, mem_len) == 0);
    ASSERT_TRUE(page_->dirty);
    ASSERT_TRUE('X' == page_->getRowAndCell(3, 2).cell->contentCodepoint());

    /* The node still owns the same compressed representation, and neither its
     * storage accounting nor its discarded raw mapping changed while cloning. */
    ASSERT_TRUE(Node::Storage::compressed == node->storage());
    ASSERT_TRUE(statsEq(stats, s->memoryStats()));
    ASSERT_TRUE(retained_ptr == node->metadata()->memory);
    ASSERT_TRUE(allZero(node->metadata()->memory, mem_len));
    ASSERT_TRUE(memcmp(expected_encoded, node->data.compressed.encoded, enc_len) == 0);

    preserved.deinit();
    free(expected_encoded);
    free(expected);
}

TEST(page_list, PageList_memory_stats_include_unused_pool_backing) {
    ListHolder s(opts(80, 24));

    /* Pool allocation ownership is based on the requested layout fitting in a
     * standard item. The Page itself exposes only the initialized prefix. */
    Node *node = s->createPage(PageList::CreatePage(Capacity(1, 1)));
    ASSERT_TRUE(Node::Owned::pool == node->owned);
    ASSERT_TRUE(node->page()->memory_len < PageList::PagePool::item_size);
    node->page()->size.rows = 1;
    s->pages.append(node);
    s->total_rows += 1;

    const size_t raw_len = node->metadata()->memory_len;
    const PageList::MemoryStats before = s->memoryStats();
    ASSERT_TRUE(before.raw_bytes < s->page_size);
    ASSERT_TRUE(before.raw_bytes == before.resident_raw_bytes);
    ASSERT_TRUE(s->page_size == before.resident_backing_bytes);
    ASSERT_TRUE(s->page_size == before.estimatedResidentBytes());

    ASSERT_TRUE(s->compressPage(node));
    const size_t encoded_len = node->data.compressed.encoded_len;
    const PageList::MemoryStats compressed = s->memoryStats();
    ASSERT_TRUE(before.raw_bytes == compressed.raw_bytes);
    ASSERT_TRUE(before.resident_raw_bytes - raw_len == compressed.resident_raw_bytes);
    ASSERT_TRUE(raw_len == compressed.decommitted_raw_bytes);
    ASSERT_TRUE(before.resident_backing_bytes - raw_len == compressed.resident_backing_bytes);
    ASSERT_TRUE(encoded_len == compressed.encoded_bytes);
    ASSERT_TRUE(before.estimatedResidentBytes() - raw_len + encoded_len == compressed.estimatedResidentBytes());
}

TEST(page_list, PageList_does_not_compress_the_mixed_history_and_active_page) {
    ListHolder s(opts(80, 24));

    /* One additional row creates history, but the history and all active rows
     * still share the first page. The active boundary therefore has a
     * historical prefix and must remain resident as one indivisible mapping. */
    (void)growNode(&*s);
    const Pin active = s->getTopLeft(point::Tag::active);
    ASSERT_TRUE(s->pages.first == active.node);
    ASSERT_TRUE(active.y > 0);

    (void)s->compress(CMode::full);
    ASSERT_FALSE(s->pages.first->isCompressed());
}

TEST(page_list, PageList_compresses_only_complete_cold_history_pages) {
    const zigstd::Allocator alloc = talloc();

    /* More active rows than one page at these dimensions can hold ensures the
     * active area spans multiple nodes when the pass chooses its boundary. */
    const size::CellCountInt active_rows = (size::CellCountInt)(PageList::initialCapacity(80).rows + 1);
    ListHolder s(alloc, opts(80, active_rows));
    ASSERT_TRUE(growColdPagesForTest(&*s, 2));

    /* Move the active top into the boundary page so it has both a historical
     * prefix and active rows while the active area still spans later pages. */
    (void)growNode(&*s);

    const Pin active = s->getTopLeft(point::Tag::active);
    Node *active_node = active.node;
    ASSERT_TRUE(active.y > 0);
    ASSERT_TRUE(active_node != s->pages.last);

    size_t expected_compressed = 0;
    size_t expected_raw_bytes = 0;
    for (Node *node = s->pages.first; node; node = node->next) {
        if (node == active_node) break;
        expected_compressed += 1;
        expected_raw_bytes += node->metadata()->memory_len;
    }
    ASSERT_TRUE(2 == expected_compressed);

    Node *first = s->pages.first;
    *first->page()->getRowAndCell(0, 0).cell = page::Cell::init('X');
    uint8_t *expected = dupeMem(first->page());
    uint8_t *first_memory = first->page()->memory;
    const size_t page_size = s->page_size;

    (void)s->compress(CMode::full);
    const PageList::MemoryStats memory = s->memoryStats();
    ASSERT_TRUE(expected_compressed == memory.compressed_pages);
    ASSERT_TRUE(expected_raw_bytes == memory.decommitted_raw_bytes);
    ASSERT_TRUE(memory.encoded_bytes < memory.decommitted_raw_bytes);
    ASSERT_TRUE(page_size == s->page_size);

    size_t actual_encoded_bytes = 0;
    for (Node *node = s->pages.first; node; node = node->next) {
        if (node == active_node) break;
        ASSERT_TRUE(node->isCompressed());
        actual_encoded_bytes += node->data.compressed.encoded_len;
    }
    ASSERT_TRUE(actual_encoded_bytes == memory.encoded_bytes);
    for (Node *node = active_node; node; node = node->next) {
        ASSERT_FALSE(node->isCompressed());
    }

    /* Restoring the oldest page preserves both the mapping identity and all
     * of its bytes even though the pass discarded its physical pages. */
    ASSERT_TRUE(first_memory == first->metadata()->memory);
    ASSERT_TRUE(memcmp(expected, first->page()->memory, first->page()->memory_len) == 0);
    ASSERT_TRUE(first_memory == first->page()->memory);
    free(expected);
}

TEST(page_list, PageList_lazily_restores_compressed_history_made_active_by_resize) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 1));

    Node *first = s->pages.first;
    *first->page()->getRowAndCell(0, 0).cell = page::Cell::init('X');
    uint8_t *memory_ptr = first->page()->memory;
    const size_t memory_len = first->page()->memory_len;
    const size_t page_size = s->page_size;

    (void)s->compress(CMode::full);
    ASSERT_TRUE(first->isCompressed());

    /* Pull all scrollback into the active area by making the viewport as tall
     * as the complete screen. A row-only resize needs only page metadata, so
     * the newly active page can remain compressed until its contents are used. */
    const size::CellCountInt all_rows = (size::CellCountInt)s->total_rows;
    {
        PageList::Resize r;
        r.rows = all_rows;
        ASSERT_TRUE(s->resize(r));
    }
    const Pin active = s->getTopLeft(point::Tag::active);
    ASSERT_TRUE(first == active.node);
    ASSERT_TRUE(0 == active.y);
    ASSERT_TRUE(first->isCompressed());
    ASSERT_TRUE(page_size == s->page_size);

    /* The compression pass must not reconsider the node now that it is active.
     * Content access follows the normal page boundary, which recommits and
     * restores the retained mapping before returning the cell. */
    (void)s->compress(CMode::full);
    ASSERT_TRUE(first->isCompressed());
    const PageList::Cell cell = s->getCell(Point::active()).value;
    ASSERT_TRUE('X' == cell.cell->contentCodepoint());
    ASSERT_FALSE(first->isCompressed());
    ASSERT_TRUE(memory_ptr == first->page()->memory);
    ASSERT_TRUE(memory_len == first->page()->memory_len);
    ASSERT_TRUE(page_size == s->page_size);
}

TEST(page_list, PageList_full_and_incremental_compression_skip_a_spanning_viewport) {
    ListHolder full(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*full, 3));

    ListHolder incremental(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*incremental, 3));

    /* Start near the end of the first page so the viewport intersects both
     * the first and second historical page mappings. */
    Node *first = full->pages.first;
    const size_t overlap_rows = full->rows / 2;
    const size_t viewport_row = first->rows() - overlap_rows;
    full->scroll(PageList::Scroll::rowAt(viewport_row));
    incremental->scroll(PageList::Scroll::rowAt(viewport_row));
    ASSERT_TRUE(full->getTopLeft(point::Tag::viewport).node !=
                full->getBottomRight(point::Tag::viewport).value.node);
    Node *second = first->next;
    ASSERT_TRUE(first == full->getTopLeft(point::Tag::viewport).node);
    ASSERT_TRUE(second == full->getBottomRight(point::Tag::viewport).value.node);

    (void)full->compress(CMode::full);
    (void)incremental->compress(CMode::drain);
    ASSERT_TRUE(statsEq(full->memoryStats(), incremental->memoryStats()));
    ASSERT_FALSE(first->isCompressed());
    ASSERT_FALSE(second->isCompressed());

    Node *full_active = full->getTopLeft(point::Tag::active).node;
    Node *incremental_active = incremental->getTopLeft(point::Tag::active).node;
    Node *full_node = full->pages.first;
    Node *incremental_node = incremental->pages.first;
    while (full_node != full_active) {
        ASSERT_TRUE(full_node->isCompressed() == incremental_node->isCompressed());

        full_node = full_node->next;
        incremental_node = incremental_node->next;
    }
    ASSERT_TRUE(incremental_active == incremental_node);

    PageList::CompressionIterator eligible = PageList::CompressionIterator::init(&*full);
    size_t compressed_pages = 0;
    while (Node *node = eligible.next()) {
        compressed_pages += 1;
        ASSERT_TRUE(node->isCompressed());
    }
    ASSERT_TRUE(compressed_pages > 0);
}

TEST(page_list, PageList_cold_compression_continues_after_an_incompressible_page) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(growColdPagesForTest(&*s, 2));

    Node *first = s->pages.first;
    Node *second = first->next;
    uint8_t *original = dupeMem(first->page());
    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(0x434F4C4450414745ull);
    zigstd::Random(&prng).bytes(first->page()->memory, first->page()->memory_len);

    const size_t page_size = s->page_size;
    (void)s->compress(CMode::full);
    const PageList::MemoryStats memory = s->memoryStats();
    ASSERT_TRUE(1 == memory.compressed_pages);
    ASSERT_FALSE(first->isCompressed());
    ASSERT_TRUE(second->isCompressed());
    ASSERT_TRUE(second->metadata()->memory_len == memory.decommitted_raw_bytes);
    ASSERT_TRUE(memory.encoded_bytes < memory.decommitted_raw_bytes);
    ASSERT_TRUE(page_size == s->page_size);

    /* Failed resident candidates are deliberately retried on later passes,
     * while the successful page remains compressed and is skipped. */
    (void)s->compress(CMode::full);
    ASSERT_TRUE(statsEq(memory, s->memoryStats()));

    /* Wisp: restore the valid page bytes before teardown. */
    memcpy(first->page()->memory, original, first->page()->memory_len);
    free(original);
}

TEST(page_list, PageList_compression_restores_through_page_access) {
    const zigstd::Allocator alloc = talloc();
    ListHolder s(alloc, opts(80, 24));

    Node *node = s->pages.first;
    Page *page = node->page();
    page->dirty = true;
    *page->getRowAndCell(3, 2).cell = page::Cell::init('X');

    uint8_t *expected = dupeMem(page);
    uint8_t *memory_ptr = page->memory;
    const size_t memory_len = page->memory_len;
    const size_t page_size = s->page_size;

    ASSERT_TRUE(s->compressPage(node));
    ASSERT_TRUE(node->isCompressed());
    ASSERT_TRUE(24 == node->rows());
    ASSERT_TRUE(80 == node->cols());
    ASSERT_TRUE(memory_ptr == node->metadata()->memory);
    ASSERT_TRUE(memory_len == node->metadata()->memory_len);
    ASSERT_TRUE(page_size == s->page_size);

    /* Pin access restores the page without changing its retained mapping. */
    const Pin page_pin = Pin::at(node, 3, 2);
    ASSERT_TRUE('X' == page_pin.rowAndCell().cell->contentCodepoint());
    ASSERT_FALSE(node->isCompressed());
    ASSERT_TRUE(memory_ptr == node->page()->memory);
    ASSERT_TRUE(memcmp(expected, node->page()->memory, memory_len) == 0);
    ASSERT_TRUE(node->page()->dirty);

    /* Recompressing exercises reuse of the page-pool scratch item. Page
     * iterator chunks also restore before exposing row memory. */
    ASSERT_TRUE(s->compressPage(node));
    PageList::PageIterator page_it = Pin(node).pageIterator(PageList::Direction::right_down, Maybe<Pin>());
    PageList::Chunk chunk;
    ASSERT_TRUE(page_it.next(&chunk));
    size_t chunk_len;
    (void)chunk.rows(&chunk_len);
    ASSERT_TRUE(node->rows() == chunk_len);
    ASSERT_FALSE(node->isCompressed());
    ASSERT_TRUE(memcmp(expected, node->page()->memory, memory_len) == 0);

    /* Read-only PageList operations restore through the same boundary. */
    ASSERT_TRUE(s->compressPage(node));
    PageList cloned;
    ASSERT_TRUE(s->clone(alloc, PageList::Clone(Point::screen()), &cloned) == page::PageError::none);
    ASSERT_FALSE(node->isCompressed());
    ASSERT_TRUE('X' == cloned.pages.first->page()->getRowAndCell(3, 2).cell->contentCodepoint());
    cloned.deinit();
    free(expected);
}

TEST(page_list, PageList_compression_uses_temporary_scratch_for_oversized_pages) {
    const zigstd::Allocator alloc = talloc();
    ListHolder s(alloc, opts(80, 24));

    Node *node = s->pages.first;
    while (node->page()->memory_len <= PageList::std_size) {
        ASSERT_TRUE(s->increaseCapacity(node, PageList::IncreaseCapacity::grapheme_bytes, &node) ==
                    PageList::IncreaseCapacityError::none);
    }

    uint8_t *expected = dupeMem(node->page());
    uint8_t *memory_ptr = node->page()->memory;
    const size_t memory_len = node->page()->memory_len;
    const size_t page_size = s->page_size;

    ASSERT_TRUE(s->compressPage(node));
    ASSERT_TRUE(node->isCompressed());
    ASSERT_TRUE(page_size == s->page_size);
    ASSERT_TRUE(memory_ptr == node->metadata()->memory);
    ASSERT_TRUE(memory_len == node->metadata()->memory_len);

    ASSERT_TRUE(memcmp(expected, node->page()->memory, memory_len) == 0);
    ASSERT_FALSE(node->isCompressed());
    ASSERT_TRUE(memory_ptr == node->page()->memory);
    free(expected);
}

TEST(page_list, PageList_compression_leaves_incompressible_pages_resident) {
    const zigstd::Allocator alloc = talloc();
    ListHolder s(alloc, opts(80, 24));

    Node *node = s->pages.first;
    uint8_t *original = dupeMem(node->page());

    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(0x504147454C495354ull);
    zigstd::Random(&prng).bytes(node->page()->memory, node->page()->memory_len);
    const size_t page_size = s->page_size;

    ASSERT_FALSE(s->compressPage(node));
    ASSERT_FALSE(node->isCompressed());
    ASSERT_TRUE(page_size == s->page_size);

    memcpy(node->page()->memory, original, node->page()->memory_len);
    free(original);
}

TEST(page_list, PageList_reset_discards_malformed_compressed_data) {
    ListHolder s(opts(80, 24));

    Node *node = s->pages.first;
    ASSERT_TRUE(s->compressPage(node));
    memset(node->data.compressed.encoded, 0xFF, node->data.compressed.encoded_len);

    s->reset();
    ASSERT_FALSE(s->pages.first->isCompressed());
    ASSERT_TRUE(1 == s->totalPages());
}

TEST(page_list, PageList_deinit_discards_malformed_compressed_data) {
    PageList s;
    ASSERT_TRUE(PageList::init(talloc(), opts(80, 24), &s));
    Node *node = s.pages.first;
    ASSERT_TRUE(s.compressPage(node));
    memset(node->data.compressed.encoded, 0xFF, node->data.compressed.encoded_len);

    s.deinit();
}

typedef PageList::Scrollbar Scrollbar;

static bool sbEq(const Scrollbar &a, size_t total, size_t offset, size_t len) {
    return a.total == total && a.offset == offset && a.len == len;
}

/* expectEqual on Pin compares every field. */
static bool pinEq(const Pin &a, const Pin &b) { return a.eql(b) && a.garbage == b.garbage; }

static size::CellCountInt stdMaxCols() {
    size::CellCountInt c;
    const bool ok = page::std_capacity().maxCols(&c);
    assert(ok);
    (void)ok;
    return c;
}

static Capacity stdAdjust(size::CellCountInt cols) {
    Capacity c;
    const bool ok = page::std_capacity().adjust(Capacity::Adjustment::withCols(cols), &c);
    assert(ok);
    (void)ok;
    return c;
}

static Point cellScreenPoint(PageList &s, const Point &pt) { return s.getCell(pt).value.screenPoint(); }

TEST(page_list, PageList_prune_reuses_malformed_compressed_page_memory) {
    ListHolder s(opts(80, 24, 2 * PageList::PagePool::item_size));

    /* Allocate the second page so the first one can be pruned and reused. */
    while (s->pages.first == s->pages.last) (void)growNode(&*s);
    Node *first = s->pages.first;
    ASSERT_TRUE(s->compressPage(first));
    memset(first->data.compressed.encoded, 0xFF, first->data.compressed.encoded_len);

    bool reused = false;
    const size_t growth_limit = (size_t)s->pages.last->capacity().rows + 1;
    for (size_t i = 0; i < growth_limit; i++) {
        if (Node *new_node = growNode(&*s)) {
            if (new_node == first) {
                reused = true;
                break;
            }
        }
    }

    ASSERT_TRUE(reused);
    ASSERT_TRUE(first == s->pages.last);
    ASSERT_FALSE(first->isCompressed());
    ASSERT_TRUE(1 == first->rows());
    first->page()->assertIntegrity();
}

TEST(page_list, PageList) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Initial total rows should be our row count */
    ASSERT_TRUE(s->rows == s->total_rows);

    /* Our viewport pin must be defined. It isn't used until the
     * viewport is a pin but it prevents undefined access on clone. */
    ASSERT_TRUE(s->viewport_pin->node == s->pages.first);

    /* Active area should be the top */
    ASSERT_TRUE(pinEq(Pin(s->pages.first, 0, 0), s->getTopLeft(point::Tag::active)));

    /* Scrollbar should be where we expect it */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->rows, 0, s->rows));
}

TEST(page_list, PageList_init_error) {
    /* Test every failure point in `init` and ensure that we don't
     * leak memory (testing.allocator verifies) since we're exiting early. */
    const PageList::InitTw init_tags[] = {PageList::InitTw::init_memory_pool, PageList::InitTw::init_pages,
                                          PageList::InitTw::viewport_pin, PageList::InitTw::viewport_pin_track};
    for (PageList::InitTw tag : init_tags) {
        typedef PageList::init_tw tw;
        tw::errorAlways(tag, AllocTw::OutOfMemory);
        PageList s;
        ASSERT_FALSE(PageList::init(talloc(), opts(80, 24), &s));
        (void)tw::end(tripwire::ResetMode::reset);
    }

    /* init calls initPages transitively, so let's check that if
     * any failures happen in initPages, we also don't leak memory. */
    const PageList::InitPagesTw page_tags[] = {PageList::InitPagesTw::page_node, PageList::InitPagesTw::page_buf_std,
                                               PageList::InitPagesTw::page_buf_non_std};
    for (PageList::InitPagesTw tag : page_tags) {
        typedef PageList::initPages_tw tw;
        tw::errorAlways(tag, AllocTw::OutOfMemory);

        const size::CellCountInt cols =
            tag == PageList::InitPagesTw::page_buf_std ? 80 : (size::CellCountInt)(stdMaxCols() + 1);
        PageList s;
        ASSERT_FALSE(PageList::init(talloc(), opts(cols, 24), &s));
        (void)tw::end(tripwire::ResetMode::reset);
    }

    /* Try non-standard pages since they don't go in our pool. */
    {
        typedef PageList::initPages_tw tw;
        tw::errorAfter(PageList::InitPagesTw::page_buf_non_std, AllocTw::OutOfMemory, 1);
        PageList s;
        ASSERT_FALSE(PageList::init(talloc(),
                                    opts((size::CellCountInt)(stdMaxCols() + 1),
                                         (size::CellCountInt)(page::std_capacity().rows + 1)),
                                    &s));
        (void)tw::end(tripwire::ResetMode::reset);
    }
}

TEST(page_list, PageList_init_rows_across_two_pages) {
    /* Find a cap that makes it so that rows don't fit on one page. */
    const size::CellCountInt rows = 100;
    Capacity cap = stdAdjust(50);
    while (cap.rows >= rows) cap = stdAdjust((size::CellCountInt)(cap.cols + 50));

    /* Init */
    ListHolder s(opts(cap.cols, rows));
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Initial total rows should be our row count */
    ASSERT_TRUE(s->rows == s->total_rows);

    /* Scrollbar should be where we expect it */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->rows, 0, s->rows));
}

TEST(page_list, PageList_init_more_than_max_cols) {
    /* Initialize with more columns than we can fit in our standard
     * capacity. This is going to force us to go to a non-standard page
     * immediately. */
    ListHolder s(opts((size::CellCountInt)(stdMaxCols() + 1), 80));
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* We expect a single, non-standard page */
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE(s->pages.first->page()->memory_len > PageList::std_size);

    /* Initial total rows should be our row count */
    ASSERT_TRUE(s->rows == s->total_rows);

    /* Scrollbar should be where we expect it */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->rows, 0, s->rows));
}

TEST(page_list, PageList_pointFromPin_active_no_history) {
    ListHolder s(opts(80, 24));

    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::active, Pin(s->pages.first, 0, 0)), Point::active(0, 0)));
    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::active, Pin(s->pages.first, 2, 4)), Point::active(4, 2)));
}

TEST(page_list, PageList_pointFromPin_active_with_history) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(30));

    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::active, Pin(s->pages.first, 30, 2)), Point::active(2, 0)));

    /* In history, invalid */
    ASSERT_FALSE(s->pointFromPin(point::Tag::active, Pin(s->pages.first, 21, 2)).has);
}

static void growPagesPaused(PageList &s, size_t n) {
    Node *cur_page = s.pages.last;
    cur_page->page()->pauseIntegrityChecks(true);
    for (size_t i = 0; i < n; i++) {
        if (Node *new_page = growNode(&s)) {
            cur_page->page()->pauseIntegrityChecks(false);
            cur_page = new_page;
            cur_page->page()->pauseIntegrityChecks(true);
        }
    }
    cur_page->page()->pauseIntegrityChecks(false);
}

TEST(page_list, PageList_pointFromPin_active_from_prior_page) {
    ListHolder s(opts(80, 24));
    /* Grow so we take up at least 5 pages. */
    Page *page = s->pages.last->page();
    growPagesPaused(*s, (size_t)page->capacity.rows * 5);

    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::active, Pin(s->pages.last, 0, 2)), Point::active(2, 0)));

    /* Prior page */
    ASSERT_FALSE(s->pointFromPin(point::Tag::active, Pin(s->pages.first, 0, 0)).has);
}

TEST(page_list, PageList_pointFromPin_traverse_pages) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up at least 2 pages. */
    Page *page = s->pages.last->page();
    const size_t page_cap = page->capacity.rows;
    growPagesPaused(*s, page_cap * 2);

    {
        const size_t pages = s->totalPages();
        const size_t expected_y = page_cap * (pages - 2) + 5;

        ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::screen, Pin(s->pages.last->prev, 5, 2)),
                            Point::screen(2, (uint32_t)expected_y)));
    }

    /* Prior page */
    ASSERT_FALSE(s->pointFromPin(point::Tag::active, Pin(s->pages.first, 0, 0)).has);
}

TEST(page_list, PageList_pointFromPin_rejects_overflowing_screen_coordinate) {
    /* Use maximum-height metadata-only pages to model a valid scrollback just
     * beyond the u32 coordinate range without allocating their backing cells. */
    const size_t page_count = 65539;
    const size::CellCountInt rows_per_page = 0xFFFF;
    Node *nodes = (Node *)calloc(page_count, sizeof(Node));

    for (size_t i = 0; i < page_count; i++) {
        Node *node = &nodes[i];
        node->prev = i > 0 ? &nodes[i - 1] : nullptr;
        node->next = i + 1 < page_count ? &nodes[i + 1] : nullptr;
        node->data.tag = Node::Data::Tag::resident;
        node->serial = i;
        node->owned = Node::Owned::heap;
        node->data.resident().size.cols = 1;
        node->data.resident().size.rows = rows_per_page;
    }

    PageList s;
    s.pages.first = &nodes[0];
    s.pages.last = &nodes[page_count - 1];

    ASSERT_FALSE(s.pointFromPin(point::Tag::screen, Pin(&nodes[page_count - 1], 0, 0)).has);
    free(nodes);
}

TEST(page_list, PageList_active_after_grow) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE((size_t)s->rows + 10 == s->totalRows());

    /* Make sure all points make sense */
    ASSERT_TRUE(cellScreenPoint(*s, Point::viewport()).eql(Point::screen(0, 10)));
    ASSERT_TRUE(cellScreenPoint(*s, Point::screen()).eql(Point::screen(0, 0)));
    ASSERT_TRUE(cellScreenPoint(*s, Point::active()).eql(Point::screen(0, 10)));

    /* Scrollbar should be in the active area */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 10, s->rows));
}

TEST(page_list, PageList_grow_allows_exceeding_max_size_for_active_area) {
    /* Setup our initial page so that we fully take up one page. */
    const Capacity cap = stdAdjust(5);
    ListHolder s(opts(5, cap.rows, (size_t)0));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Grow once because we guarantee at least two pages of
     * capacity so we want to get to that. */
    (void)growNode(&*s);
    const size_t start_pages = s->totalPages();
    ASSERT_TRUE(start_pages >= 2);

    /* Surgically modify our pages so that they have a smaller size. */
    {
        for (Node *page = s->pages.first; page; page = page->next) {
            page->page()->size.rows = 1;
            page->page()->capacity.rows = 1;
        }

        /* Avoid integrity check failures */
        s->total_rows = s->totalRows();
    }

    /* Grow our row and ensure we don't prune pages because we need
     * enough for the active area. */
    (void)growNode(&*s);
    ASSERT_TRUE(start_pages + 1 == s->totalPages());
}

TEST(page_list, PageList_grow_prune_required_with_a_single_page) {
    /* Need scrollback > 0 to have a scrollbar to test */
    ListHolder s(opts(80, 24));

    /* This block is all test setup. There is nothing required about this
     * behavior during a refactor. This is setting up a scenario that is
     * possible to trigger a bug (#2280). */
    {
        /* Increase our capacity until our page is larger than the standard size.
         * This is important because it triggers a scenario where our calculated
         * minSize() which is supposed to accommodate 2 pages is no longer true. */
        for (;;) {
            const Page::Layout layout = Page::layout(s->pages.first->capacity());
            if (layout.total_size > PageList::std_size) break;
            Node *n;
            ASSERT_TRUE(s->increaseCapacity(s->pages.first, PageList::IncreaseCapacity::grapheme_bytes, &n) ==
                        PageList::IncreaseCapacityError::none);
        }
        ASSERT_TRUE(s->pages.first != nullptr);
        ASSERT_TRUE(s->pages.first == s->pages.last);
    }

    /* Figure out the remaining number of rows. This is the amount that
     * can be added to the current page before we need to allocate a new
     * page. */
    const size_t rem = (size_t)s->pages.first->capacity().rows - s->pages.first->rows();
    for (size_t i = 0; i < rem; i++) ASSERT_TRUE(growNode(&*s) == nullptr);

    /* The next one we add will trigger a new page. */
    Node *new_ = growNode(&*s);
    ASSERT_TRUE(new_ != nullptr);
    ASSERT_TRUE(new_ != s->pages.first);

    /* Scrollbar should be in the active area */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_scrollbar_with_max_size_0_after_grow) {
    ListHolder s(opts(80, 24, (size_t)0));

    /* Grow some rows (simulates normal terminal output) */
    ASSERT_TRUE(s->growRows(10));

    const Scrollbar sb = s->scrollbar();

    /* With no scrollback (max_size = 0), total should equal rows */
    ASSERT_TRUE(s->rows == sb.total);

    /* With no scrollback, offset should be 0 (nowhere to scroll back to) */
    ASSERT_TRUE(0 == sb.offset);
}

TEST(page_list, PageList_scroll_with_max_size_0_no_history) {
    ListHolder s(opts(80, 24, (size_t)0));

    ASSERT_TRUE(s->growRows(10));

    /* Remember initial viewport position */
    const Point pt_before = cellScreenPoint(*s, Point::viewport());

    /* Try to scroll backwards into "history" - should be no-op */
    s->scroll(PageList::Scroll::deltaRow(-5));
    ASSERT_TRUE(s->viewport == Viewport::active);

    /* Scroll to top - should also be no-op with no scrollback */
    s->scroll(PageList::Scroll::top());
    const Point pt_after = cellScreenPoint(*s, Point::viewport());
    ASSERT_TRUE(pt_before.eql(pt_after));
}

TEST(page_list, PageList_scroll_top) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    ASSERT_TRUE(cellScreenPoint(*s, Point::viewport()).eql(Point::screen(0, 10)));

    s->scroll(PageList::Scroll::top());

    ASSERT_TRUE(cellScreenPoint(*s, Point::viewport()).eql(Point::screen(0, 0)));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 0, s->rows));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(cellScreenPoint(*s, Point::viewport()).eql(Point::screen(0, 0)));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 0, s->rows));

    s->scroll(PageList::Scroll::active());
    ASSERT_TRUE(cellScreenPoint(*s, Point::viewport()).eql(Point::screen(0, 20)));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows, s->rows));
}

/* Wisp: `s.getCell(.{ .viewport = .{} }).?.screenPoint() == .{ .screen = .{ .x = 0, .y = y } }` */
static bool vpAt(PageList &s, uint32_t y) { return cellScreenPoint(s, Point::viewport()).eql(Point::screen(0, y)); }
typedef PageList::Scroll Scroll;

TEST(page_list, PageList_scroll_delta_row_back) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    ASSERT_TRUE(vpAt(*s, 10));

    s->scroll(Scroll::deltaRow(-1));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows - 1, s->rows));

    ASSERT_TRUE(vpAt(*s, 9));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 9));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows - 11, s->rows));

    s->scroll(Scroll::deltaRow(-1));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows - 12, s->rows));
}

TEST(page_list, PageList_scroll_delta_row_back_overflow) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    ASSERT_TRUE(vpAt(*s, 10));

    s->scroll(Scroll::deltaRow(-100));

    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 0, s->rows));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 0, s->rows));
}

TEST(page_list, PageList_scroll_minimum_row_delta) {
    ListHolder s(opts(10, 3));

    /* Create one row of history so scrolling all the way back has an
     * observable result. */
    ASSERT_TRUE(s->growRows(1));
    s->scroll(Scroll::deltaRow(PTRDIFF_MIN));

    ASSERT_TRUE(Viewport::top == s->viewport);
}

TEST(page_list, PageList_scroll_delta_row_forward) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    ASSERT_TRUE(vpAt(*s, 10));

    s->scroll(Scroll::top());
    s->scroll(Scroll::deltaRow(2));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 2, s->rows));

    ASSERT_TRUE(vpAt(*s, 2));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 2));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 2, s->rows));
}

TEST(page_list, PageList_scroll_delta_row_forward_into_active) {
    ListHolder s(opts(80, 24));

    s->scroll(Scroll::deltaRow(2));

    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_scroll_delta_row_back_without_space_preserves_active) {
    ListHolder s(opts(80, 24));
    s->scroll(Scroll::deltaRow(-1));

    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(s->viewport == Viewport::active);

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_scroll_to_pin) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    s->scroll(Scroll::pinAt(s->pin(Point::screen(2, 4)).value));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 4, s->rows));

    ASSERT_TRUE(vpAt(*s, 4));

    s->scroll(Scroll::pinAt(s->pin(Point::screen(2, 5)).value));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 5, s->rows));

    ASSERT_TRUE(vpAt(*s, 5));
}

TEST(page_list, PageList_scroll_to_pin_in_active) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    s->scroll(Scroll::pinAt(s->pin(Point::screen(2, 30)).value));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), s->total_rows - s->rows, s->rows));

    ASSERT_TRUE(vpAt(*s, 10));
}

TEST(page_list, PageList_scroll_to_pin_at_top) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    s->scroll(Scroll::pinAt(s->pin(Point::screen(2, 0)).value));

    ASSERT_TRUE(s->viewport == Viewport::top);

    ASSERT_TRUE(sbEq(s->scrollbar(), s->totalRows(), 0, s->rows));

    ASSERT_TRUE(vpAt(*s, 0));
}

TEST(page_list, PageList_scroll_to_row_0) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    ASSERT_TRUE(vpAt(*s, 10));

    s->scroll(Scroll::rowAt(0));
    ASSERT_TRUE(s->viewport == Viewport::top);

    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 0, s->rows));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 0, s->rows));
}

TEST(page_list, PageList_scroll_to_row_in_scrollback) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(20));

    ASSERT_TRUE(vpAt(*s, 20));

    s->scroll(Scroll::rowAt(5));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 5, s->rows));

    ASSERT_TRUE(vpAt(*s, 5));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 5));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 5, s->rows));
}

TEST(page_list, PageList_scroll_to_row_in_middle) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(50));

    const size_t total = s->total_rows;
    const size_t midpoint = total / 2;
    s->scroll(Scroll::rowAt(midpoint));

    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, midpoint, s->rows));

    ASSERT_TRUE(vpAt(*s, (uint32_t)midpoint));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, (uint32_t)midpoint));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, midpoint, s->rows));
}

TEST(page_list, PageList_scroll_to_row_at_active_boundary) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(20));

    const size_t active_start = s->total_rows - s->rows;

    s->scroll(Scroll::rowAt(active_start));

    ASSERT_TRUE(s->viewport == Viewport::active);

    ASSERT_TRUE(vpAt(*s, (uint32_t)active_start));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));

    ASSERT_TRUE(s->growRows(10));

    ASSERT_TRUE(s->viewport == Viewport::active);

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_scroll_to_row_beyond_active) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(10));

    s->scroll(Scroll::rowAt(1000));

    ASSERT_TRUE(s->viewport == Viewport::active);

    ASSERT_TRUE(vpAt(*s, 10));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_scroll_to_row_without_scrollback) {
    ListHolder s(opts(80, 24));

    s->scroll(Scroll::rowAt(5));

    ASSERT_TRUE(s->viewport == Viewport::active);

    ASSERT_TRUE(vpAt(*s, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_scroll_to_row_then_delta) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(30));

    s->scroll(Scroll::rowAt(10));

    ASSERT_TRUE(s->viewport == Viewport::pin);

    ASSERT_TRUE(vpAt(*s, 10));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 10, s->rows));

    s->scroll(Scroll::deltaRow(5));

    ASSERT_TRUE(s->viewport == Viewport::pin);

    ASSERT_TRUE(vpAt(*s, 15));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 15, s->rows));

    s->scroll(Scroll::deltaRow(-3));

    ASSERT_TRUE(s->viewport == Viewport::pin);

    ASSERT_TRUE(vpAt(*s, 12));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 12, s->rows));
}

TEST(page_list, PageList_scroll_to_row_with_cache_fast_path_down) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(50));

    s->scroll(Scroll::rowAt(10));

    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 10, s->rows));

    ASSERT_TRUE(vpAt(*s, 10));

    /* Verify cache is populated */
    ASSERT_TRUE(s->viewport_pin_row_offset.has);
    ASSERT_TRUE(10 == s->viewport_pin_row_offset.value);

    /* Now scroll to a different row - this should use the fast path */
    s->scroll(Scroll::rowAt(20));

    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 20, s->rows));

    ASSERT_TRUE(vpAt(*s, 20));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 20));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 20, s->rows));
}

TEST(page_list, PageList_scroll_to_row_with_cache_fast_path_up) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(50));

    s->scroll(Scroll::rowAt(30));

    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 30, s->rows));

    ASSERT_TRUE(vpAt(*s, 30));

    /* Verify cache is populated */
    ASSERT_TRUE(s->viewport_pin_row_offset.has);
    ASSERT_TRUE(30 == s->viewport_pin_row_offset.value);

    /* Now scroll up to a different row - this should use the fast path */
    s->scroll(Scroll::rowAt(15));

    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 15, s->rows));

    ASSERT_TRUE(vpAt(*s, 15));

    ASSERT_TRUE(s->growRows(10));
    ASSERT_TRUE(vpAt(*s, 15));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 15, s->rows));
}

TEST(page_list, PageList_scroll_clear) {
    ListHolder s(opts(80, 24));

    *s->getCell(Point::active(0, 0)).value.cell = page::Cell::init('A');
    *s->getCell(Point::active(0, 1)).value.cell = page::Cell::init('A');

    ASSERT_TRUE(s->scrollClear());

    ASSERT_TRUE(vpAt(*s, 2));
}

static void setPrompt(Page *page, size_t y, page::Row::SemanticPrompt sp) {
    page->getRowAndCell(0, y).row->setSemanticPrompt(sp);
}

TEST(page_list, PageList_jump_zero_prompts) {
    ListHolder s(opts(5, 3));
    ASSERT_TRUE(s->growRows(3));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    setPrompt(page, 1, page::Row::SemanticPrompt::prompt);
    setPrompt(page, 5, page::Row::SemanticPrompt::prompt);

    s->scroll(Scroll::deltaPrompt(0));
    ASSERT_TRUE(s->viewport == Viewport::active);

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));
}

TEST(page_list, PageList_jump_minimum_prompt_delta) {
    ListHolder s(opts(10, 3));

    s->scroll(Scroll::deltaPrompt(PTRDIFF_MIN));
    ASSERT_TRUE(Viewport::active == s->viewport);
}

static bool viewportScreenIs(PageList &s, uint32_t y) {
    return pointEq(s.pointFromPin(point::Tag::screen, s.pin(Point::viewport()).value), Point::screen(0, y));
}

TEST(page_list, Screen_jump_back_one_prompt) {
    ListHolder s(opts(5, 3));
    ASSERT_TRUE(s->growRows(3));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    setPrompt(page, 1, page::Row::SemanticPrompt::prompt);
    setPrompt(page, 5, page::Row::SemanticPrompt::prompt);

    /* Jump back */
    {
        s->scroll(Scroll::deltaPrompt(-1));
        ASSERT_TRUE(s->viewport == Viewport::pin);
        ASSERT_TRUE(viewportScreenIs(*s, 1));

        ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 1, s->rows));
    }
    {
        s->scroll(Scroll::deltaPrompt(-1));
        ASSERT_TRUE(s->viewport == Viewport::pin);
        ASSERT_TRUE(viewportScreenIs(*s, 1));

        ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 1, s->rows));
    }

    /* Jump forward */
    {
        s->scroll(Scroll::deltaPrompt(1));
        ASSERT_TRUE(s->viewport == Viewport::active);
        ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));
    }
    {
        s->scroll(Scroll::deltaPrompt(1));
        ASSERT_TRUE(s->viewport == Viewport::active);
        ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, s->total_rows - s->rows, s->rows));
    }
}

TEST(page_list, Screen_jump_forward_prompt_skips_multiline_continuation) {
    ListHolder s(opts(5, 3));
    ASSERT_TRUE(s->growRows(7));

    /* Multiline prompt on rows 1-3. */
    s->pin(Point::screen(0, 1)).value.rowAndCell().row->setSemanticPrompt(page::Row::SemanticPrompt::prompt);
    s->pin(Point::screen(0, 2))
        .value.rowAndCell()
        .row->setSemanticPrompt(page::Row::SemanticPrompt::prompt_continuation);
    s->pin(Point::screen(0, 3))
        .value.rowAndCell()
        .row->setSemanticPrompt(page::Row::SemanticPrompt::prompt_continuation);

    /* Next prompt after command output. */
    s->pin(Point::screen(0, 6)).value.rowAndCell().row->setSemanticPrompt(page::Row::SemanticPrompt::prompt);

    /* Starting at the first prompt line should jump to the next prompt,
     * not to continuation lines. */
    s->scroll(Scroll::rowAt(1));
    s->scroll(Scroll::deltaPrompt(1));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(viewportScreenIs(*s, 6));

    /* Starting in the middle of continuation lines should also jump to
     * the next prompt. */
    s->scroll(Scroll::rowAt(2));
    s->scroll(Scroll::deltaPrompt(1));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(viewportScreenIs(*s, 6));
}

TEST(page_list, PageList_grow_fit_in_capacity) {
    ListHolder s(opts(80, 24));

    /* So we know we're using capacity to grow */
    Page *last = s->pages.last->page();
    ASSERT_TRUE(last->size.rows < last->capacity.rows);

    /* Grow */
    ASSERT_TRUE(growNode(&*s) == nullptr);
    ASSERT_TRUE(cellScreenPoint(*s, Point::active()).eql(Point::screen(0, 1)));
}

TEST(page_list, PageList_grow_allocate) {
    ListHolder s(opts(80, 24));

    /* Grow to capacity */
    Node *last_node = s->pages.last;
    Page *last = s->pages.last->page();
    const size_t n = (size_t)last->capacity.rows - last->size.rows;
    for (size_t i = 0; i < n; i++) {
        ASSERT_TRUE(growNode(&*s) == nullptr);
    }

    /* Grow, should allocate */
    Node *new_ = growNode(&*s);
    ASSERT_TRUE(new_ != nullptr);
    ASSERT_TRUE(s->pages.last == new_);
    ASSERT_TRUE(last_node->next == new_);
    {
        const PageList::Cell cell = s->getCell(Point::active(0, (uint32_t)(s->rows - 1))).value;
        ASSERT_TRUE(cell.node == new_);
        ASSERT_TRUE(Point::screen(0, last->capacity.rows).eql(cell.screenPoint()));
    }
}

TEST(page_list, PageList_Cell_screenPoint_supports_long_scrollback) {
    /* A modest number of full-size page nodes is enough to exceed the u16
     * row range without allocating any page backing memory. screenPoint only
     * reads the linked metadata while calculating the absolute coordinate. */
    const size_t page_count = 307;
    const size::CellCountInt rows_per_page = page::std_capacity().rows;
    Node *nodes = (Node *)calloc(page_count, sizeof(Node));

    for (size_t i = 0; i < page_count; i++) {
        Node *node = &nodes[i];
        node->prev = i > 0 ? &nodes[i - 1] : nullptr;
        node->next = i + 1 < page_count ? &nodes[i + 1] : nullptr;
        node->data.tag = Node::Data::Tag::resident;
        node->serial = i;
        node->owned = Node::Owned::heap;
        node->data.resident().size.cols = 1;
        node->data.resident().size.rows = rows_per_page;
    }

    const uint32_t expected_y = (uint32_t)(page_count - 1) * (uint32_t)rows_per_page;
    ASSERT_TRUE(expected_y > 0xFFFF);

    PageList::Cell cell;
    cell.node = &nodes[page_count - 1];
    cell.row = nullptr;
    cell.cell = nullptr;
    cell.row_idx = 0;
    cell.col_idx = 0;
    ASSERT_TRUE(Point::screen(0, expected_y).eql(cell.screenPoint()));
    free(nodes);
}

TEST(page_list, PageList_set_max_bytes_prunes_immediately_and_can_be_raised) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;
    const size_t item = PageList::PagePool::item_size;

    ListHolder s(opts(cols, 1));

    /* Build four complete pages of history followed by the active row. */
    ASSERT_TRUE(s->growRows(4 * page_rows));
    ASSERT_TRUE(5 == s->totalPages());

    Node *removed = s->pages.first;
    Node *retained = s->pages.last->prev;
    Pin *removed_pin = s->trackPin(Pin(removed));
    Pin *retained_pin = s->trackPin(Pin(retained));

    s->scroll(Scroll::pinAt(*retained_pin));
    ASSERT_TRUE(3 * page_rows == s->scrollbar().offset);

    /* The active-area minimum is two pages. Lowering below that immediately
     * removes all older complete historical pages. */
    s->setMaxBytes(item);
    ASSERT_TRUE(item == s->limits.bytes.explicit_);
    ASSERT_TRUE(2 * item == s->limits.max(PageList::Limits::Key::bytes));
    ASSERT_TRUE(s->limits.max(PageList::Limits::Key::bytes) == s->page_size);
    ASSERT_TRUE(2 == s->totalPages());
    ASSERT_TRUE(page_rows == s->total_rows - s->rows);
    ASSERT_TRUE(retained == s->pages.first);
    ASSERT_TRUE(retained == removed_pin->node);
    ASSERT_TRUE(removed_pin->garbage);
    ASSERT_TRUE(retained == retained_pin->node);
    ASSERT_FALSE(retained_pin->garbage);
    ASSERT_TRUE(0 == s->scrollbar().offset);

    /* Raising the limit doesn't allocate or otherwise change retained data,
     * but subsequent growth can exceed the previous effective limit. */
    const size_t limited_size = s->page_size;
    const size_t limited_rows = s->total_rows;
    s->setMaxBytes(8 * item);
    ASSERT_TRUE(limited_size == s->page_size);
    ASSERT_TRUE(limited_rows == s->total_rows);
    ASSERT_TRUE(s->growRows(2 * page_rows));
    ASSERT_TRUE(s->page_size > limited_size);

    /* Null restores unlimited growth and likewise preserves current data. */
    const size_t raised_size = s->page_size;
    const size_t raised_rows = s->total_rows;
    s->setMaxBytes(Maybe<size_t>());
    ASSERT_TRUE(SIZE_MAX == s->limits.bytes.explicit_);
    ASSERT_TRUE(raised_size == s->page_size);
    ASSERT_TRUE(raised_rows == s->total_rows);
    ASSERT_TRUE(s->growRows(5 * page_rows));
    ASSERT_TRUE(s->page_size > 8 * item);

    s->untrackPin(retained_pin);
    s->untrackPin(removed_pin);
}

TEST(page_list, PageList_set_max_bytes_zero_preserves_active_boundary) {
    ListHolder s(opts(80, 1));

    /* Make the sole page larger than the effective zero-byte limit. Its first
     * row will be history, but the same indivisible page also contains active. */
    while (s->page_size <= s->limits.bytes.min) {
        Node *n;
        ASSERT_TRUE(s->increaseCapacity(s->pages.first, PageList::IncreaseCapacity::grapheme_bytes, &n) ==
                    PageList::IncreaseCapacityError::none);
    }
    (void)growNode(&*s);
    ASSERT_TRUE(1 == s->totalPages());
    ASSERT_TRUE(s->pages.first == s->getTopLeft(point::Tag::active).node);
    ASSERT_TRUE(s->getTopLeft(point::Tag::active).y > 0);

    s->scroll(Scroll::top());
    ASSERT_TRUE(s->viewport == Viewport::top);

    s->setMaxBytes((size_t)0);
    ASSERT_TRUE(0 == s->limits.bytes.explicit_);
    ASSERT_TRUE(s->page_size > s->limits.max(PageList::Limits::Key::bytes));
    ASSERT_TRUE(1 == s->totalPages());
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->rows, 0, s->rows));

    /* No-scrollback mode cannot be moved back into the retained boundary row. */
    s->scroll(Scroll::top());
    ASSERT_TRUE(s->viewport == Viewport::active);
}

typedef PageList::Limits Limits;
static const size_t kItem = PageList::PagePool::item_size;

/* Wisp: `.{ .cols = c, .rows = r, .reflow = f }` (negative = null). */
static PageList::Resize rz(int cols = -1, int rows = -1, bool reflow = true) {
    PageList::Resize r;
    if (cols >= 0) r.cols = (size::CellCountInt)cols;
    if (rows >= 0) r.rows = (size::CellCountInt)rows;
    r.reflow = reflow;
    return r;
}

static bool limitsEq(const Limits &a, const Limits &b) {
    return a.bytes.explicit_ == b.bytes.explicit_ && a.bytes.min == b.bytes.min &&
           a.lines.explicit_ == b.lines.explicit_ && a.lines.min == b.lines.min;
}

/* Wisp: grow `n` rows, tracking the most recently allocated page. */
static void growTracking(PageList &s, size_t n) {
    for (size_t i = 0; i < n; i++) (void)growNode(&s);
}

TEST(page_list, PageList_set_max_lines_prunes_immediately_and_can_be_raised) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;
    const size_t lowered_lines = page_rows + page_rows / 2;

    ListHolder s(opts(cols, 1));

    ASSERT_TRUE(s->growRows(4 * page_rows));
    ASSERT_TRUE(5 == s->totalPages());

    Node *removed = s->pages.first;
    Node *retained = s->pages.last->prev;
    Pin *removed_pin = s->trackPin(Pin(removed));
    Pin *retained_pin = s->trackPin(Pin(retained));

    s->scroll(Scroll::pinAt(*retained_pin));
    ASSERT_TRUE(3 * page_rows == s->scrollbar().offset);

    /* Whole-page enforcement undershoots a non-page-aligned line limit. */
    s->setMaxLines(lowered_lines);
    ASSERT_TRUE(lowered_lines == s->limits.lines.explicit_);
    ASSERT_TRUE(lowered_lines == s->limits.max(Limits::Key::lines));
    ASSERT_TRUE(page_rows == s->total_rows - s->rows);
    ASSERT_TRUE(2 == s->totalPages());
    ASSERT_TRUE(retained == s->pages.first);
    ASSERT_TRUE(retained == removed_pin->node);
    ASSERT_TRUE(removed_pin->garbage);
    ASSERT_TRUE(retained == retained_pin->node);
    ASSERT_FALSE(retained_pin->garbage);
    ASSERT_TRUE(0 == s->scrollbar().offset);

    const size_t limited_size = s->page_size;
    const size_t limited_rows = s->total_rows;
    s->setMaxLines(4 * page_rows);
    ASSERT_TRUE(limited_size == s->page_size);
    ASSERT_TRUE(limited_rows == s->total_rows);
    ASSERT_TRUE(s->growRows(2 * page_rows));
    ASSERT_TRUE(s->total_rows - s->rows > lowered_lines);

    const size_t raised_size = s->page_size;
    const size_t raised_rows = s->total_rows;
    s->setMaxLines(Maybe<size_t>());
    ASSERT_TRUE(SIZE_MAX == s->limits.lines.explicit_);
    ASSERT_TRUE(raised_size == s->page_size);
    ASSERT_TRUE(raised_rows == s->total_rows);
    ASSERT_TRUE(s->growRows(3 * page_rows));
    ASSERT_TRUE(s->total_rows - s->rows > 4 * page_rows);

    s->untrackPin(retained_pin);
    s->untrackPin(removed_pin);
}

TEST(page_list, PageList_set_max_limits_remain_independent) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;
    const size_t byte_limit = 3 * kItem;
    const size_t line_limit = page_rows / 2;

    ListHolder s(opts(cols, 1));

    ASSERT_TRUE(s->growRows(4 * page_rows));

    /* The byte setter leaves the line limit unlimited. */
    s->setMaxBytes(byte_limit);
    ASSERT_TRUE(byte_limit == s->limits.bytes.explicit_);
    ASSERT_TRUE(SIZE_MAX == s->limits.lines.explicit_);
    ASSERT_TRUE(3 == s->totalPages());
    ASSERT_TRUE(2 * page_rows == s->total_rows - s->rows);

    /* The smaller runtime line limit prunes one more complete page without
     * changing the configured byte limit. Its effective value is raised to
     * the existing one-page minimum. */
    s->setMaxLines(line_limit);
    ASSERT_TRUE(byte_limit == s->limits.bytes.explicit_);
    ASSERT_TRUE(line_limit == s->limits.lines.explicit_);
    ASSERT_TRUE(page_rows == s->limits.max(Limits::Key::lines));
    ASSERT_TRUE(2 == s->totalPages());
    ASSERT_TRUE(page_rows == s->total_rows - s->rows);

    /* Removing only the line limit leaves byte enforcement in effect. */
    s->setMaxLines(Maybe<size_t>());
    ASSERT_TRUE(s->growRows(3 * page_rows));
    ASSERT_TRUE(byte_limit == s->page_size);
    ASSERT_TRUE(3 == s->totalPages());
    ASSERT_TRUE(s->total_rows - s->rows > page_rows);
}

TEST(page_list, PageList_max_lines_uses_one_page_minimum) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;

    ListHolder s(opts(cols, 1, Maybe<size_t>(), page_rows / 2));

    ASSERT_TRUE(page_rows == s->limits.max(Limits::Key::lines));

    /* The requested limit is below one page, so a complete page of history
     * remains valid. */
    ASSERT_TRUE(s->growRows(page_rows));
    ASSERT_TRUE(page_rows == s->total_rows - s->rows);
    ASSERT_TRUE(2 == s->totalPages());

    Node *first = s->pages.first;
    const size_t old_page_size = s->page_size;

    /* One more row puts us over the effective limit. The now-complete
     * historical page is removed rather than partially trimmed. */
    (void)growNode(&*s);
    ASSERT_TRUE(1 == s->total_rows - s->rows);
    ASSERT_TRUE(1 == s->totalPages());
    ASSERT_TRUE(s->pages.first != first);
    ASSERT_TRUE(old_page_size - kItem == s->page_size);
}

TEST(page_list, PageList_max_lines_does_not_round_larger_limits) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;
    const size_t max_lines = page_rows + page_rows / 2;

    ListHolder s(opts(cols, 1, Maybe<size_t>(), max_lines));

    ASSERT_TRUE(max_lines == s->limits.max(Limits::Key::lines));
    ASSERT_TRUE(s->growRows(max_lines));
    ASSERT_TRUE(max_lines == s->total_rows - s->rows);

    Node *first = s->pages.first;
    Node *retained = first->next;
    Pin *removed_pin = s->trackPin(Pin(first));
    Pin *retained_pin = s->trackPin(Pin(retained));

    s->scroll(Scroll::pinAt(*retained_pin));
    ASSERT_TRUE(page_rows == s->scrollbar().offset);

    const size_t old_page_size = s->page_size;
    (void)growNode(&*s);

    /* Whole-page pruning undershoots the requested limit without rounding it. */
    ASSERT_TRUE(max_lines + 1 - page_rows == s->total_rows - s->rows);
    ASSERT_TRUE(retained == s->pages.first);
    ASSERT_TRUE(retained == removed_pin->node);
    ASSERT_TRUE(removed_pin->garbage);
    ASSERT_TRUE(retained == retained_pin->node);
    ASSERT_FALSE(retained_pin->garbage);
    ASSERT_TRUE(0 == s->scrollbar().offset);
    ASSERT_TRUE(old_page_size - kItem == s->page_size);

    s->untrackPin(retained_pin);
    s->untrackPin(removed_pin);
}

TEST(page_list, PageList_max_lines_and_max_size_enforce_the_smaller_limit) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;

    /* A line limit of one page keeps the logical allocation below a much
     * larger byte limit. */
    {
        ListHolder s(opts(cols, 1, 8 * kItem, page_rows));

        ASSERT_TRUE(s->growRows(4 * page_rows));
        ASSERT_TRUE(s->total_rows - s->rows <= s->limits.max(Limits::Key::lines));
        ASSERT_TRUE(s->totalPages() <= 2);
        ASSERT_TRUE(s->page_size < s->limits.max(Limits::Key::bytes));
    }

    /* A two-page byte limit prunes before the larger line limit is reached. */
    {
        ListHolder s(opts(cols, 1, kItem, 4 * page_rows));

        ASSERT_TRUE(s->growRows(2 * page_rows));
        ASSERT_TRUE(s->total_rows - s->rows < s->limits.max(Limits::Key::lines));
        ASSERT_TRUE(s->limits.max(Limits::Key::bytes) == s->page_size);
    }
}

TEST(page_list, PageList_max_lines_applies_to_resize_and_clone) {
    const size::CellCountInt cols = 80;
    const size_t page_rows = PageList::initialCapacity(cols).rows;

    ListHolder s(opts(cols, 2, Maybe<size_t>(), page_rows));

    ASSERT_TRUE(s->growRows(page_rows));
    ASSERT_TRUE(page_rows == s->total_rows - s->rows);

    /* Prevent row shrinking from trimming the trailing active row instead of
     * turning it into history. */
    *s->getCell(Point::active(0, 1)).value.cell = page::Cell::init('A');

    ASSERT_TRUE(s->resize(rz(-1, 1, false)));
    ASSERT_TRUE(1 == s->total_rows - s->rows);

    const size::CellCountInt new_cols = cols + 1;
    ASSERT_TRUE(s->resize(rz(new_cols, -1, true)));
    ASSERT_TRUE(Limits::minMaxLines(new_cols) == s->limits.lines.min);

    /* Exercise the same active-row shrink through the reflow path. Reflow
     * completes before the newly historical complete page is pruned. */
    {
        ListHolder reflowed(opts(cols, 2, Maybe<size_t>(), page_rows));

        ASSERT_TRUE(reflowed->growRows(page_rows));
        *reflowed->getCell(Point::active(0, 1)).value.cell = page::Cell::init('A');

        ASSERT_TRUE(reflowed->resize(rz(new_cols, 1, true)));
        ASSERT_TRUE(Limits::minMaxLines(new_cols) == reflowed->limits.lines.min);
        ASSERT_TRUE(reflowed->total_rows - reflowed->rows <= reflowed->limits.max(Limits::Key::lines) ||
                    reflowed->pages.first == reflowed->getTopLeft(point::Tag::active).node);
    }

    PageList cloned;
    ASSERT_TRUE(s->clone(talloc(), PageList::Clone(Point::screen()), &cloned) == page::PageError::none);

    ASSERT_TRUE(limitsEq(s->limits, cloned.limits));

    ASSERT_TRUE(cloned.growRows(2 * cloned.limits.max(Limits::Key::lines)));
    ASSERT_TRUE(cloned.total_rows - cloned.rows <= cloned.limits.max(Limits::Key::lines) ||
                cloned.pages.first == cloned.getTopLeft(point::Tag::active).node);
    cloned.deinit();
}

TEST(page_list, PageList_grow_prune_scrollback) {
    /* Use std_size to limit scrollback so pruning is triggered. */
    ListHolder s(opts(80, 24, PageList::std_size));

    /* Grow to capacity */
    Node *page1_node = s->pages.last;
    Page *page1 = page1_node->page();
    {
        const size_t n = (size_t)page1->capacity.rows - page1->size.rows;
        for (size_t i = 0; i < n; i++) ASSERT_TRUE(growNode(&*s) == nullptr);
    }

    /* Grow and allocate one more page. Then fill that page up. */
    Node *page2_node = growNode(&*s);
    ASSERT_TRUE(page2_node != nullptr);
    Page *page2 = page2_node->page();
    {
        const size_t n = (size_t)page2->capacity.rows - page2->size.rows;
        for (size_t i = 0; i < n; i++) ASSERT_TRUE(growNode(&*s) == nullptr);
    }

    /* Get our page size */
    const size_t old_page_size = s->page_size;

    /* Create a tracked pin in the first page */
    Pin *p = s->trackPin(s->pin(Point::screen()).value);
    ASSERT_TRUE(p->node == s->pages.first);

    /* Scroll back to create a pinned viewport (not active) */
    const size_t pin_y = page1->capacity.rows / 2;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);

    /* Get the scrollbar state to populate the cache */
    const Scrollbar scrollbar_before = s->scrollbar();
    ASSERT_TRUE(pin_y == scrollbar_before.offset);

    /* Next should create a new page, but it should reuse our first
     * page since we're at max size. */
    Node *new_ = growNode(&*s);
    ASSERT_TRUE(new_ != nullptr);
    ASSERT_TRUE(s->pages.last == new_);
    ASSERT_TRUE(s->page_size == old_page_size);

    /* Our first should now be page2 and our last should be page1 */
    ASSERT_TRUE(page2_node == s->pages.first);
    ASSERT_TRUE(page1_node == s->pages.last);

    /* Our tracked pin should point to the top-left of the first page */
    ASSERT_TRUE(p->node == s->pages.first);
    ASSERT_TRUE(p->x == 0);
    ASSERT_TRUE(p->y == 0);
    ASSERT_TRUE(p->garbage);

    /* Verify the viewport offset cache was invalidated. After pruning,
     * the offset should have changed because we removed rows from
     * the beginning. */
    {
        const Scrollbar scrollbar_after = s->scrollbar();
        const size_t rows_pruned = page1->capacity.rows;
        const size_t expected_offset = pin_y >= rows_pruned ? pin_y - rows_pruned : 0;
        ASSERT_TRUE(expected_offset == scrollbar_after.offset);
    }
    s->untrackPin(p);
}

TEST(page_list, PageList_grow_prune_scrollback_with_viewport_pin_not_in_pruned_page) {
    /* Use std_size to limit scrollback so pruning is triggered. */
    ListHolder s(opts(80, 24, PageList::std_size));

    /* Grow to capacity of first page */
    Node *page1_node = s->pages.last;
    Page *page1 = page1_node->page();
    {
        const size_t n = (size_t)page1->capacity.rows - page1->size.rows;
        for (size_t i = 0; i < n; i++) ASSERT_TRUE(growNode(&*s) == nullptr);
    }

    /* Grow and allocate second page, then fill it up */
    Node *page2_node = growNode(&*s);
    ASSERT_TRUE(page2_node != nullptr);
    Page *page2 = page2_node->page();
    {
        const size_t n = (size_t)page2->capacity.rows - page2->size.rows;
        for (size_t i = 0; i < n; i++) ASSERT_TRUE(growNode(&*s) == nullptr);
    }

    /* Get our page size */
    const size_t old_page_size = s->page_size;

    /* Scroll back to create a pinned viewport in page2 (NOT page1)
     * This is the key difference from the previous test - the viewport
     * pin is NOT in the page that will be pruned. */
    const size_t pin_y = (size_t)page1->capacity.rows + 5;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(s->viewport_pin->node == page2_node);

    /* Get the scrollbar state to populate the cache */
    const Scrollbar scrollbar_before = s->scrollbar();
    ASSERT_TRUE(pin_y == scrollbar_before.offset);

    /* Next grow will trigger pruning of the first page.
     * The viewport_pin.node is page2, not page1, so it won't be moved
     * by the pin update loop, but the cached offset still needs to be
     * invalidated because rows were removed from the beginning. */
    Node *new_ = growNode(&*s);
    ASSERT_TRUE(new_ != nullptr);
    ASSERT_TRUE(s->pages.last == new_);
    ASSERT_TRUE(s->page_size == old_page_size);

    /* Our first should now be page2 (page1 was pruned) */
    ASSERT_TRUE(page2_node == s->pages.first);

    /* The viewport pin should still be on page2, unchanged */
    ASSERT_TRUE(s->viewport_pin->node == page2_node);

    /* Verify the viewport offset cache was invalidated/updated.
     * After pruning, the offset should have decreased by the number
     * of rows that were pruned. */
    const Scrollbar scrollbar_after = s->scrollbar();
    const size_t rows_pruned = page1->capacity.rows;
    const size_t expected_offset = pin_y - rows_pruned;
    ASSERT_TRUE(expected_offset == scrollbar_after.offset);
}

TEST(page_list, PageList_eraseRows_invalidates_viewport_offset_cache) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up several pages worth of history */
    Page *page = s->pages.last->page();
    const size_t cap_rows = page->capacity.rows;
    growTracking(*s, cap_rows * 3);

    /* Scroll back to create a pinned viewport somewhere in the middle
     * of the scrollback */
    const size_t pin_y = cap_rows;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Erase some history rows BEFORE the viewport pin.
     * This removes rows from before our pin, which changes its absolute
     * offset from the top, but the cache is not invalidated. */
    const size_t rows_to_erase = cap_rows / 2;
    s->eraseHistory(Point::history(0, (uint32_t)(rows_to_erase - 1)));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y - rows_to_erase, s->rows));
}

TEST(page_list, PageList_eraseRow_invalidates_viewport_offset_cache) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up several pages worth of history */
    Page *page = s->pages.last->page();
    const size_t cap_rows = page->capacity.rows;
    growTracking(*s, cap_rows * 3);

    /* Scroll back to create a pinned viewport somewhere in the middle
     * of the scrollback */
    const size_t pin_y = cap_rows;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Erase a single row from the history BEFORE the viewport pin.
     * This removes one row from before our pin, which changes its absolute
     * offset from the top by 1, but the cache is not invalidated. */
    s->eraseRow(Point::history(0, 0));

    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y - 1, s->rows));
}

TEST(page_list, PageList_eraseRowBounded_invalidates_viewport_offset_cache) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up several pages worth of history */
    Page *page = s->pages.last->page();
    growTracking(*s, (size_t)page->capacity.rows * 3);

    /* Scroll back to create a pinned viewport somewhere in the middle
     * of the scrollback */
    const uint16_t pin_y = 4;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Erase a row from the history BEFORE the viewport pin with a bounded
     * shift. This removes one row from before our pin, which changes its
     * absolute offset from the top by 1, but the cache is not invalidated. */
    s->eraseRowBounded(Point::history(0, 0), 10);

    /* Verify the scrollbar reflects the change (offset decreased by 1) */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y - 1, s->rows));
}

typedef PageList::IncreaseCapacity IncCap;
typedef PageList::IncreaseCapacityError IncErr;

static Node *incCap(PageList &s, Node *node, Maybe<IncCap> adj) {
    Node *out = nullptr;
    const IncErr e = s.increaseCapacity(node, adj, &out);
    assert(e == IncErr::none);
    (void)e;
    return out;
}

static void fillCodepoints(PageList &s, Page *page, uint32_t add) {
    for (size_t y = 0; y < s.rows; y++) {
        for (size_t x = 0; x < s.cols; x++) {
            *page->getRowAndCell(x, y).cell = page::Cell::init((uint32_t)x + add);
        }
    }
}

static bool codepointsAre(PageList &s, Page *page, uint32_t add) {
    for (size_t y = 0; y < s.rows; y++) {
        for (size_t x = 0; x < s.cols; x++) {
            if (page->getRowAndCell(x, y).cell->contentCodepoint() != (uint32_t)x + add) return false;
        }
    }
    return true;
}

TEST(page_list, PageList_row_erasure_renews_affected_page_generations) {
    ListHolder s(opts(80, 24));
    while (s->totalPages() < 2) (void)growNode(&*s);

    Node *first = s->pages.first;
    Node *second = first->next;
    uint64_t first_serial = first->serial;
    uint64_t second_serial = second->serial;
    uint64_t activity = s->page_compression.activity_serial;

    s->eraseRow(Point::history(0, 0));
    ASSERT_FALSE(s->nodeIsValid(first, first_serial));
    ASSERT_FALSE(s->nodeIsValid(second, second_serial));
    ASSERT_TRUE(activity != s->page_compression.activity_serial);

    first_serial = first->serial;
    second_serial = second->serial;
    activity = s->page_compression.activity_serial;
    s->eraseRowBounded(Point::history(0, 0), (size_t)first->rows() + 1);
    ASSERT_FALSE(s->nodeIsValid(first, first_serial));
    ASSERT_FALSE(s->nodeIsValid(second, second_serial));
    ASSERT_TRUE(activity != s->page_compression.activity_serial);
}

TEST(page_list, PageList_trailing_row_truncation_renews_page_generation) {
    ListHolder s(opts(80, 24));

    Node *node = s->pages.last;
    const uint64_t old_serial = node->serial;
    const size::CellCountInt trimmed = s->trimTrailingBlankRows(1);
    s->total_rows -= trimmed;
    ASSERT_TRUE(1 == trimmed);
    ASSERT_FALSE(s->nodeIsValid(node, old_serial));

    (void)growNode(&*s);
    ASSERT_TRUE(expectLivePageSerialsValidForTest(&*s));
}

TEST(page_list, PageList_eraseRowBounded_multi_page_invalidates_viewport_offset_cache) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up several pages worth of history */
    const size_t cap_rows = s->pages.last->page()->capacity.rows;
    growTracking(*s, cap_rows * 3);

    /* Scroll back to create a pinned viewport somewhere in the middle
     * of the scrollback, after the first page */
    const size_t pin_y = cap_rows + 1;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Erase a row from the beginning of history with a limit that spans
     * across multiple pages. This ensures we hit the code path where
     * eraseRowBounded finds the limit boundary in a subsequent page. */
    const size_t limit = cap_rows + 10;
    s->eraseRowBounded(Point::history(0, 0), limit);

    /* Verify the scrollbar reflects the change (offset decreased by 1) */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y - 1, s->rows));
}

TEST(page_list, PageList_eraseRowBounded_full_page_shift_invalidates_viewport_offset_cache) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up several pages worth of history */
    const size_t cap_rows = s->pages.last->page()->capacity.rows;
    growTracking(*s, cap_rows * 4);

    /* Scroll back to create a pinned viewport somewhere well beyond
     * the first two pages */
    const size_t pin_y = 5;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Erase a row from the beginning of history with a limit that is
     * larger than multiple full pages. This ensures we hit the code path
     * where eraseRowBounded continues looping through entire pages,
     * rotating all rows in each page until it reaches the limit or
     * runs out of pages. */
    const size_t limit = cap_rows * 2 + 10;
    s->eraseRowBounded(Point::history(0, 0), limit);

    /* Verify the scrollbar reflects the change (offset decreased by 1) */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y - 1, s->rows));
}

TEST(page_list, PageList_eraseRowBounded_exhausts_pages_invalidates_viewport_offset_cache) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up several pages worth of history */
    const size_t cap_rows = s->pages.last->page()->capacity.rows;
    growTracking(*s, cap_rows * 3);

    /* Our total rows should include history */
    const size_t total_rows_before = s->totalRows();
    ASSERT_TRUE(total_rows_before > s->rows);

    /* Scroll back to create a pinned viewport somewhere in the history,
     * well after the erase will complete */
    const size_t pin_y = cap_rows * 2 + 10;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Erase a row from the beginning of history with a limit that is
     * LARGER than all remaining pages combined. This ensures we exhaust
     * all pages in the while loop and reach the cleanup code after the loop. */
    const size_t limit = total_rows_before * 2;
    s->eraseRowBounded(Point::history(0, 0), limit);

    /* Verify the scrollbar reflects the change (offset decreased by 1) */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y - 1, s->rows));
}

TEST(page_list, PageList_increaseCapacity_to_increase_styles) {
    ListHolder s(opts(2, 2, (size_t)0));

    const size_t original_styles_cap = s->pages.first->capacity().styles;

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        /* Write all our data so we can assert its the same after */
        fillCodepoints(*s, s->pages.first->page(), 0);
    }

    /* Increase our styles */
    (void)incCap(*s, s->pages.first, IncCap::styles);

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* Verify capacity doubled */
        ASSERT_TRUE(original_styles_cap * 2 == page->capacity.styles);

        /* Verify data preserved */
        ASSERT_TRUE(codepointsAre(*s, page, 0));
    }
}

TEST(page_list, PageList_increaseCapacity_styles_projects_capacity_from_page_density) {
    ListHolder s(opts(2, 2, (size_t)0));

    const size_t original_cap = s->pages.first->capacity().styles;

    /* Write styled cells so the page has a measurable per-row style
     * density (unlike the plain doubling test above, which grows a
     * page with no styles in use). */
    style::Style bold;
    bold.flags.bold = true;
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        for (size_t y = 0; y < s->rows; y++) {
            for (size_t x = 0; x < s->cols; x++) {
                const Page::RowAndCell rac = page->getRowAndCell(x, y);
                style::Id style_id;
                ASSERT_TRUE(page->styles.add((const void *)page->memory, bold, &style_id) ==
                            ref_counted_set::AddError::none);
                rac.row->setStyled(true);
                page::Cell c = page::Cell::init((uint32_t)x + 1);
                c.setStyleId(style_id);
                *rac.cell = c;
            }
        }
    }

    (void)incCap(*s, s->pages.first, IncCap::styles);

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* The page uses only two active rows out of thousands of rows
         * of capacity, so the projected full-page need saturates the
         * 32x-per-event growth bound instead of merely doubling. */
        ASSERT_TRUE(original_cap * 32 == page->capacity.styles);

        /* All cell content and styles are preserved by the growth. */
        for (size_t y = 0; y < s->rows; y++) {
            for (size_t x = 0; x < s->cols; x++) {
                const Page::RowAndCell rac = page->getRowAndCell(x, y);
                ASSERT_TRUE((uint32_t)x + 1 == rac.cell->contentCodepoint());
                ASSERT_TRUE(rac.cell->style_id() != style::default_id);
                ASSERT_TRUE(bold.eql(*page->styles.get((const void *)page->memory, rac.cell->style_id())));
            }
        }
    }
}

TEST(page_list, PageList_increaseCapacity_to_increase_graphemes) {
    ListHolder s(opts(2, 2, (size_t)0));

    const size_t original_cap = s->pages.first->capacity().grapheme_bytes;

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        fillCodepoints(*s, s->pages.first->page(), 0);
    }

    (void)incCap(*s, s->pages.first, IncCap::grapheme_bytes);

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(original_cap * 2 == page->capacity.grapheme_bytes);

        ASSERT_TRUE(codepointsAre(*s, page, 0));
    }
}

TEST(page_list, PageList_increaseCapacity_graphemes_projects_capacity_from_page_density) {
    ListHolder s(opts(2, 2, (size_t)0));

    const size_t original_cap = s->pages.first->capacity().grapheme_bytes;

    /* Write cells with grapheme data so the page has a measurable
     * per-row grapheme density (unlike the plain doubling test above,
     * which grows a page with no grapheme usage). */
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        for (size_t y = 0; y < s->rows; y++) {
            for (size_t x = 0; x < s->cols; x++) {
                const Page::RowAndCell rac = page->getRowAndCell(x, y);
                *rac.cell = page::Cell::init((uint32_t)x + 1);
                ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0301) == page::PageError::none);
                ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0302) == page::PageError::none);
            }
        }
    }

    (void)incCap(*s, s->pages.first, IncCap::grapheme_bytes);

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* The page uses only two active rows out of thousands of rows
         * of capacity, so the projected full-page need saturates the
         * 32x-per-event growth bound instead of merely doubling. */
        ASSERT_TRUE(original_cap * 32 == page->capacity.grapheme_bytes);

        /* All cell and grapheme content is preserved by the growth. */
        ASSERT_TRUE((size_t)s->rows * s->cols == page->graphemeCount());
        for (size_t y = 0; y < s->rows; y++) {
            for (size_t x = 0; x < s->cols; x++) {
                const Page::RowAndCell rac = page->getRowAndCell(x, y);
                ASSERT_TRUE((uint32_t)x + 1 == rac.cell->contentCodepoint());
                size_t len = 0;
                const uint32_t *cps = page->lookupGrapheme(rac.cell, &len);
                ASSERT_TRUE(2 == len);
                ASSERT_TRUE(0x0301 == cps[0]);
                ASSERT_TRUE(0x0302 == cps[1]);
            }
        }
    }
}

TEST(page_list, PageList_increaseCapacity_to_increase_hyperlinks) {
    ListHolder s(opts(2, 2, (size_t)0));

    const size_t original_cap = s->pages.first->capacity().hyperlink_bytes;

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        fillCodepoints(*s, s->pages.first->page(), 0);
    }

    (void)incCap(*s, s->pages.first, IncCap::hyperlink_bytes);

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(original_cap * 2 == page->capacity.hyperlink_bytes);

        ASSERT_TRUE(codepointsAre(*s, page, 0));
    }
}

TEST(page_list, PageList_increaseCapacity_to_increase_string_bytes) {
    ListHolder s(opts(2, 2, (size_t)0));

    const size_t original_cap = s->pages.first->capacity().string_bytes;

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        fillCodepoints(*s, s->pages.first->page(), 0);
    }

    (void)incCap(*s, s->pages.first, IncCap::string_bytes);

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(original_cap * 2 == page->capacity.string_bytes);

        ASSERT_TRUE(codepointsAre(*s, page, 0));
    }
}

TEST(page_list, PageList_increaseCapacity_tracked_pins) {
    ListHolder s(opts(2, 2, (size_t)0));

    /* Create a tracked pin on the first page */
    Pin *tracked = s->trackPin(s->pin(Point::active(1, 1)).value);

    Node *old_node = s->pages.first;
    ASSERT_TRUE(old_node == tracked->node);

    /* Increase capacity */
    Node *new_node = incCap(*s, s->pages.first, IncCap::styles);

    /* Pin should now point to the new node */
    ASSERT_TRUE(new_node == tracked->node);
    ASSERT_TRUE(1 == tracked->x);
    ASSERT_TRUE(1 == tracked->y);
    s->untrackPin(tracked);
}

TEST(page_list, PageList_increaseCapacity_returns_OutOfSpace_at_max_capacity) {
    ListHolder s(opts(2, 2, (size_t)0));

    /* Keep increasing styles capacity until we get OutOfSpace */
    const size_t max_styles = (size::StyleCountInt)~(size::StyleCountInt)0;
    for (;;) {
        Node *n;
        const IncErr err = s->increaseCapacity(s->pages.first, IncCap::styles, &n);
        if (err != IncErr::none) {
            /* Before OutOfSpace, we should have reached maxInt */
            ASSERT_TRUE(IncErr::OutOfSpace == err);
            ASSERT_TRUE(max_styles == s->pages.first->capacity().styles);
            break;
        }
    }
}

TEST(page_list, PageList_increaseCapacity_after_col_shrink) {
    ListHolder s(opts(10, 2, (size_t)0));

    /* Shrink columns */
    ASSERT_TRUE(s->resize(rz(5, -1, false)));
    ASSERT_TRUE(5 == s->cols);

    {
        Page *page = s->pages.first->page();
        ASSERT_TRUE(5 == page->size.cols);
        ASSERT_TRUE(page->capacity.cols >= 10);
    }

    /* Increase capacity */
    (void)incCap(*s, s->pages.first, IncCap::styles);

    {
        Page *page = s->pages.first->page();
        /* size.cols should still be 5, not reverted to capacity.cols */
        ASSERT_TRUE(5 == page->size.cols);
        ASSERT_TRUE(5 == s->cols);
    }
}

TEST(page_list, PageList_increaseCapacity_multi_page) {
    ListHolder s(opts(80, 24));

    /* Grow to create a second page */
    Node *page1_node = s->pages.last;
    page1_node->page()->pauseIntegrityChecks(true);
    {
        const size_t n = (size_t)page1_node->capacity().rows - page1_node->rows();
        for (size_t i = 0; i < n; i++) ASSERT_TRUE(growNode(&*s) == nullptr);
    }
    page1_node->page()->pauseIntegrityChecks(false);
    ASSERT_TRUE(growNode(&*s) != nullptr);

    /* Now we have two pages */
    ASSERT_TRUE(s->pages.first != s->pages.last);
    Node *page2_node = s->pages.last;

    const size_t page1_styles_cap = s->pages.first->capacity().styles;
    const size_t page2_styles_cap = page2_node->capacity().styles;

    /* Increase capacity on the first page only */
    (void)incCap(*s, s->pages.first, IncCap::styles);

    /* First page capacity should be doubled */
    ASSERT_TRUE(page1_styles_cap * 2 == s->pages.first->capacity().styles);

    /* Second page should be unchanged */
    ASSERT_TRUE(page2_styles_cap == s->pages.last->capacity().styles);
}

TEST(page_list, PageList_increaseCapacity_preserves_dirty_flag) {
    ListHolder s(opts(2, 4, (size_t)0));

    /* Set page dirty flag and mark some rows as dirty */
    Page *page = s->pages.first->page();
    page->dirty = true;

    page::Row *rows = page->rows.ptr(page->memory);
    rows[0].setDirty(true);
    rows[1].setDirty(false);
    rows[2].setDirty(true);
    rows[3].setDirty(false);

    /* Increase capacity */
    Node *new_node = incCap(*s, s->pages.first, IncCap::styles);

    /* The page dirty flag should be preserved */
    ASSERT_TRUE(new_node->page()->dirty);

    /* Row dirty flags should be preserved */
    page::Row *new_rows = new_node->page()->rows.ptr(new_node->page()->memory);
    ASSERT_TRUE(new_rows[0].dirty());
    ASSERT_FALSE(new_rows[1].dirty());
    ASSERT_TRUE(new_rows[2].dirty());
    ASSERT_FALSE(new_rows[3].dirty());
}

TEST(page_list, PageList_pageIterator_single_page) {
    ListHolder s(opts(80, 24));

    /* The viewport should be within a single page */
    ASSERT_TRUE(s->pages.first->next == nullptr);

    /* Iterate the active area */
    PageList::PageIterator it = s->pageIterator(PageList::Direction::right_down, Point::active());
    PageList::Chunk chunk;
    {
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.first);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(s->rows == chunk.end);
    }

    /* Should only have one chunk */
    ASSERT_FALSE(it.next(&chunk));
}

static void fillFirstPagePlusOne(PageList &s) {
    Node *page1_node = s.pages.last;
    Page *page1 = page1_node->page();
    page1->pauseIntegrityChecks(true);
    const size_t n = (size_t)page1->capacity.rows - page1->size.rows;
    for (size_t i = 0; i < n; i++) (void)growNode(&s);
    page1->pauseIntegrityChecks(false);
    (void)growNode(&s);
}

TEST(page_list, PageList_pageIterator_two_pages) {
    ListHolder s(opts(80, 24));

    /* Grow to capacity */
    fillFirstPagePlusOne(*s);
    ASSERT_TRUE(s->pages.first != s->pages.last);

    /* Iterate the active area */
    PageList::PageIterator it = s->pageIterator(PageList::Direction::right_down, Point::active());
    PageList::Chunk chunk;
    {
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.first);
        const size_t start = (size_t)chunk.node->rows() - s->rows + 1;
        ASSERT_TRUE(start == chunk.start);
        ASSERT_TRUE(chunk.node->rows() == chunk.end);
    }
    {
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.last);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(1 == chunk.end);
    }
    ASSERT_FALSE(it.next(&chunk));
}

TEST(page_list, PageList_pageIterator_history_two_pages) {
    ListHolder s(opts(80, 24));

    /* Grow to capacity */
    fillFirstPagePlusOne(*s);

    /* Iterate the active area */
    PageList::PageIterator it = s->pageIterator(PageList::Direction::right_down, Point::history());
    PageList::Chunk chunk;
    {
        const Pin active_tl = s->getTopLeft(point::Tag::active);
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.first);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(active_tl.y == chunk.end);
    }
    ASSERT_FALSE(it.next(&chunk));
}

typedef PageList::Direction Dir;
typedef page::Row::SemanticPrompt SP;
typedef page::Cell::SemanticContent SC;

/* `s.pointFromPin(.screen, p).? == .{ .screen = .{ .x = x, .y = y } }` */
static bool screenAt(PageList &s, const Pin &p, uint32_t x, uint32_t y) {
    return pointEq(s.pointFromPin(point::Tag::screen, p), Point::screen((size::CellCountInt)x, y));
}

static page::Cell scCell(uint32_t cp, SC sc) {
    page::Cell c = page::Cell::init(cp);
    c.setSemanticContent(sc);
    return c;
}

TEST(page_list, PageList_pageIterator_reverse_single_page) {
    ListHolder s(opts(80, 24));

    /* The viewport should be within a single page */
    ASSERT_TRUE(s->pages.first->next == nullptr);

    /* Iterate the active area */
    PageList::PageIterator it = s->pageIterator(Dir::left_up, Point::active());
    PageList::Chunk chunk;
    {
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.first);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(s->rows == chunk.end);
    }

    /* Should only have one chunk */
    ASSERT_FALSE(it.next(&chunk));
}

TEST(page_list, PageList_pageIterator_reverse_two_pages) {
    ListHolder s(opts(80, 24));

    /* Grow to capacity */
    fillFirstPagePlusOne(*s);

    /* Iterate the active area */
    PageList::PageIterator it = s->pageIterator(Dir::left_up, Point::active());
    size_t count = 0;
    PageList::Chunk chunk;
    {
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.last);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(1 == chunk.end);
        count += (size_t)chunk.end - chunk.start;
    }
    {
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.first);
        const size_t start = (size_t)chunk.node->rows() - s->rows + 1;
        ASSERT_TRUE(start == chunk.start);
        ASSERT_TRUE(chunk.node->rows() == chunk.end);
        count += (size_t)chunk.end - chunk.start;
    }
    ASSERT_FALSE(it.next(&chunk));
    ASSERT_TRUE(s->rows == count);
}

TEST(page_list, PageList_pageIterator_reverse_history_two_pages) {
    ListHolder s(opts(80, 24));

    /* Grow to capacity */
    fillFirstPagePlusOne(*s);

    /* Iterate the active area */
    PageList::PageIterator it = s->pageIterator(Dir::left_up, Point::history());
    PageList::Chunk chunk;
    {
        const Pin active_tl = s->getTopLeft(point::Tag::active);
        ASSERT_TRUE(it.next(&chunk));
        ASSERT_TRUE(chunk.node == s->pages.first);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(active_tl.y == chunk.end);
    }
    ASSERT_FALSE(it.next(&chunk));
}

TEST(page_list, PageList_PageIterator_reverse_count_includes_row_zero) {
    ListHolder s(opts(2, 2));

    PageList::PageIterator it;
    it.row = s->getTopLeft(point::Tag::screen);
    it.limit.tag = PageList::PageIterator::Limit::Tag::count;
    it.limit.count = 1;
    it.direction = Dir::left_up;
    PageList::Chunk chunk;
    ASSERT_TRUE(it.next(&chunk));
    ASSERT_TRUE(0 == chunk.start);
    ASSERT_TRUE(1 == chunk.end);
    ASSERT_FALSE(it.next(&chunk));
}

TEST(page_list, PageList_PageIterator_count_crosses_page_boundaries) {
    ListHolder s(opts(80, 24));

    Node *first = s->pages.first;
    first->page()->pauseIntegrityChecks(true);
    while (first->rows() < first->capacity().rows) (void)growNode(&*s);
    first->page()->pauseIntegrityChecks(false);
    Node *second = growNode(&*s);
    ASSERT_TRUE(second != nullptr);

    PageList::PageIterator down;
    down.row = Pin(first, (size::CellCountInt)(first->rows() - 1));
    down.limit.tag = PageList::PageIterator::Limit::Tag::count;
    down.limit.count = 2;
    down.direction = Dir::right_down;
    PageList::Chunk chunk;
    {
        ASSERT_TRUE(down.next(&chunk));
        ASSERT_TRUE(first == chunk.node);
        ASSERT_TRUE(first->rows() - 1 == chunk.start);
        ASSERT_TRUE(first->rows() == chunk.end);
    }
    {
        ASSERT_TRUE(down.next(&chunk));
        ASSERT_TRUE(second == chunk.node);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(1 == chunk.end);
    }
    ASSERT_FALSE(down.next(&chunk));

    PageList::PageIterator up;
    up.row = Pin(second);
    up.limit.tag = PageList::PageIterator::Limit::Tag::count;
    up.limit.count = 2;
    up.direction = Dir::left_up;
    {
        ASSERT_TRUE(up.next(&chunk));
        ASSERT_TRUE(second == chunk.node);
        ASSERT_TRUE(0 == chunk.start);
        ASSERT_TRUE(1 == chunk.end);
    }
    {
        ASSERT_TRUE(up.next(&chunk));
        ASSERT_TRUE(first == chunk.node);
        ASSERT_TRUE(first->rows() - 1 == chunk.start);
        ASSERT_TRUE(first->rows() == chunk.end);
    }
    ASSERT_FALSE(up.next(&chunk));
}

TEST(page_list, PageList_cellIterator) {
    ListHolder s(opts(2, 2, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillCodepoints(*s, s->pages.first->page(), 0);

    PageList::CellIterator it = s->cellIterator(Dir::right_down, Point::screen());
    Pin p;
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 0));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 1, 0));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 1));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 1, 1));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_cellIterator_reverse) {
    ListHolder s(opts(2, 2, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillCodepoints(*s, s->pages.first->page(), 0);

    PageList::CellIterator it = s->cellIterator(Dir::left_up, Point::screen());
    Pin p;
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 1, 1));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 1));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 1, 0));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 0));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_promptIterator_left_up) {
    ListHolder s(opts(2, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    /* Normal prompt */
    setPrompt(page, 3, SP::prompt);
    /* Continuation */
    setPrompt(page, 6, SP::prompt);
    setPrompt(page, 7, SP::prompt_continuation);
    setPrompt(page, 8, SP::prompt_continuation);
    /* Broken continuation that has non-prompts in between */
    setPrompt(page, 12, SP::prompt_continuation);

    PageList::PromptIterator it = s->promptIterator(Dir::left_up, Point::screen());
    Pin p;
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 12));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 6));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 3));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_promptIterator_right_down) {
    ListHolder s(opts(2, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    /* Normal prompt */
    setPrompt(page, 3, SP::prompt);
    /* Continuation (prompt on row 6, continuation on rows 7-8) */
    setPrompt(page, 6, SP::prompt);
    setPrompt(page, 7, SP::prompt_continuation);
    setPrompt(page, 8, SP::prompt_continuation);
    /* Broken continuation that has non-prompts in between (orphaned continuation at row 12) */
    setPrompt(page, 12, SP::prompt_continuation);

    PageList::PromptIterator it = s->promptIterator(Dir::right_down, Point::screen());
    Pin p;
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 3));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 6));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 12));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_promptIterator_right_down_continuation_at_start) {
    ListHolder s(opts(2, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt continuation at row 0 (no prior rows - simulates trimmed scrollback) */
    setPrompt(page, 0, SP::prompt_continuation);
    setPrompt(page, 1, SP::prompt_continuation);
    /* Normal prompt later */
    setPrompt(page, 5, SP::prompt);

    PageList::PromptIterator it = s->promptIterator(Dir::right_down, Point::screen());
    Pin p;
    /* Should return the first continuation line since there's no prior prompt */
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 0));
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 5));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_promptIterator_right_down_with_prompt_before_continuation) {
    ListHolder s(opts(2, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 2, continuation on rows 3-4
     * Starting iteration from row 3 should still find the prompt at row 2 */
    setPrompt(page, 2, SP::prompt);
    setPrompt(page, 3, SP::prompt_continuation);
    setPrompt(page, 4, SP::prompt_continuation);

    /* Start iteration from row 3 (middle of the continuation)
     * Since we start on a continuation line, we treat it as the prompt start
     * (handles case where scrollback pruned the actual prompt) */
    PageList::PromptIterator it = s->promptIterator(Dir::right_down, Point::screen(0, 3));
    Pin p;
    /* Returns row 3 since that's the first prompt-related line we encounter */
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 3));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_promptIterator_right_down_limit_inclusive) {
    ListHolder s(opts(2, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Iterate with limit at row 5 (the prompt row) - should include it */
    PageList::PromptIterator it = s->promptIterator(Dir::right_down, Point::screen(), Point::screen(0, 5));
    Pin p;
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 5));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_promptIterator_left_up_limit_inclusive) {
    ListHolder s(opts(2, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Iterate with limit at row 10 (the prompt row) - should include it
     * tl_pt is the limit (upper bound), bl_pt is the start point for left_up */
    PageList::PromptIterator it = s->promptIterator(Dir::left_up, Point::screen(0, 10), Point::screen(0, 15));
    Pin p;
    ASSERT_TRUE(it.next(&p) && screenAt(*s, p, 0, 10));
    ASSERT_FALSE(it.next(&p));
}

TEST(page_list, PageList_highlightSemanticContent_prompt) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    {
        setPrompt(page, 5, SP::prompt);

        /* Start the prompt for the first 5 cols */
        for (size_t x = 0; x < 5; x++) *page->getRowAndCell(x, 5).cell = scCell('A', SC::prompt);

        /* Next 3 let's make input */
        for (size_t x = 5; x < 8; x++) *page->getRowAndCell(x, 5).cell = scCell('B', SC::input);
    }
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    const PageList::HighlightUntracked hl = s->highlightSemanticContent(s->pin(Point::screen(2, 5)).value, SC::prompt).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 0, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 7, 5));
}

/* Wisp: `for (x0..x1) |x| page.getRowAndCell(x, y).cell.* = .{ codepoint cp, semantic_content sc }` */
static void fillSC(Page *page, size_t y, size_t x0, size_t x1, uint32_t cp, SC sc) {
    for (size_t x = x0; x < x1; x++) *page->getRowAndCell(x, y).cell = scCell(cp, sc);
}
/* Wisp: `cell.semantic_content = sc` only. */
static void markSC(Page *page, size_t y, size_t x0, size_t x1, SC sc) {
    for (size_t x = x0; x < x1; x++) page->getRowAndCell(x, y).cell->setSemanticContent(sc);
}

static Maybe<PageList::HighlightUntracked> hlAt(PageList &s, uint32_t x, uint32_t y, SC sc) {
    return s.highlightSemanticContent(s.pin(Point::screen((size::CellCountInt)x, y)).value, sc);
}

TEST(page_list, PageList_highlightSemanticContent_prompt_with_output) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 3 cols are prompt */
    fillSC(page, 5, 0, 3, '$', SC::prompt);
    /* Next 4 are input */
    fillSC(page, 5, 3, 7, 'l', SC::input);
    /* Rest is output (shouldn't be included in prompt highlight) */
    fillSC(page, 5, 7, 10, 'o', SC::output);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting from prompt should include prompt and input, but stop at output */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::prompt).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 0, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 6, 5));
}

TEST(page_list, PageList_highlightSemanticContent_prompt_multiline) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt starts on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First row is all prompt */
    fillSC(page, 5, 0, 10, '$', SC::prompt);
    /* Row 6 continues with input */
    fillSC(page, 6, 0, 5, 'c', SC::input);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting should span both rows */
    const PageList::HighlightUntracked hl = hlAt(*s, 2, 5, SC::prompt).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 0, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 4, 6));
}

TEST(page_list, PageList_highlightSemanticContent_prompt_only) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 with only prompt content (no input) */
    setPrompt(page, 5, SP::prompt);
    fillSC(page, 5, 0, 5, '$', SC::prompt);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting should only include the prompt cells */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::prompt).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 0, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 4, 5));
}

TEST(page_list, PageList_highlightSemanticContent_prompt_to_end_of_screen) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Single prompt on row 15, no following prompt */
    setPrompt(page, 15, SP::prompt);
    fillSC(page, 15, 0, 3, '$', SC::prompt);
    fillSC(page, 15, 3, 8, 'c', SC::input);

    /* Highlighting should include prompt and input up to column 7 */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 15, SC::prompt).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 0, 15));
    ASSERT_TRUE(screenAt(*s, hl.end, 7, 15));
}

TEST(page_list, PageList_highlightSemanticContent_input_basic) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 3 cols are prompt */
    fillSC(page, 5, 0, 3, '$', SC::prompt);
    /* Next 5 are input */
    fillSC(page, 5, 3, 8, 'l', SC::input);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting input should only include input cells */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::input).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 3, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 7, 5));
}

TEST(page_list, PageList_highlightSemanticContent_input_with_output) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 2 cols are prompt */
    fillSC(page, 5, 0, 2, '$', SC::prompt);
    /* Next 3 are input */
    fillSC(page, 5, 2, 5, 'c', SC::input);
    /* Rest is output */
    fillSC(page, 5, 5, 10, 'o', SC::output);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting input should stop at output */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::input).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 2, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 4, 5));
}

TEST(page_list, PageList_highlightSemanticContent_input_multiline_with_continuation) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 2 cols are prompt */
    fillSC(page, 5, 0, 2, '$', SC::prompt);
    /* Rest is input */
    fillSC(page, 5, 2, 10, 'c', SC::input);
    /* Row 6 has continuation prompt then more input */
    fillSC(page, 6, 0, 2, '>', SC::prompt);
    fillSC(page, 6, 2, 6, 'd', SC::input);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting input should span both rows, skipping continuation prompts */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::input).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 2, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 5, 6));
}

TEST(page_list, PageList_highlightSemanticContent_input_no_input_returns_null) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 with only prompt, then immediately output */
    setPrompt(page, 5, SP::prompt);
    /* First 3 cols are prompt */
    fillSC(page, 5, 0, 3, '$', SC::prompt);
    /* Rest is output (no input!) */
    fillSC(page, 5, 3, 10, 'o', SC::output);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting input should return null when there's no input */
    ASSERT_FALSE(hlAt(*s, 0, 5, SC::input).has);
}

TEST(page_list, PageList_highlightSemanticContent_input_to_end_of_screen) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Single prompt on row 15, no following prompt */
    setPrompt(page, 15, SP::prompt);
    fillSC(page, 15, 0, 2, '$', SC::prompt);
    fillSC(page, 15, 2, 7, 'c', SC::input);

    /* Highlighting input with no following prompt */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 15, SC::input).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 2, 15));
    ASSERT_TRUE(screenAt(*s, hl.end, 6, 15));
}

TEST(page_list, PageList_highlightSemanticContent_input_prompt_only_returns_null) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 with only prompt content, no input or output */
    setPrompt(page, 5, SP::prompt);
    /* All cells are prompt */
    fillSC(page, 5, 0, 10, '$', SC::prompt);
    /* Mark rows 6-9 as prompt to ensure no input before next prompt */
    for (size_t y = 6; y < 10; y++) markSC(page, y, 0, 10, SC::prompt);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting input should return null when there's only prompts */
    ASSERT_FALSE(hlAt(*s, 0, 5, SC::input).has);
}

TEST(page_list, PageList_highlightSemanticContent_output_basic) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 2 cols are prompt */
    fillSC(page, 5, 0, 2, '$', SC::prompt);
    /* Next 3 are input */
    fillSC(page, 5, 2, 5, 'l', SC::input);
    /* Cols 5-7 are output */
    fillSC(page, 5, 5, 8, 'o', SC::output);
    /* Mark remaining cells as prompt to bound the output */
    markSC(page, 5, 8, 10, SC::prompt);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting output should only include output cells */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::output).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 5, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 7, 5));
}

TEST(page_list, PageList_highlightSemanticContent_output_multiline) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 2 cols are prompt */
    fillSC(page, 5, 0, 2, '$', SC::prompt);
    /* Next 2 are input */
    fillSC(page, 5, 2, 4, 'l', SC::input);
    /* Rest of row 5 is output */
    fillSC(page, 5, 4, 10, 'o', SC::output);
    /* Row 6 is all output */
    fillSC(page, 6, 0, 10, 'o', SC::output);
    /* Row 7 has partial output then input to bound it */
    fillSC(page, 7, 0, 5, 'o', SC::output);
    markSC(page, 7, 5, 10, SC::input);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting output should span multiple rows */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::output).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 4, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 4, 7));
}

TEST(page_list, PageList_highlightSemanticContent_output_stops_at_next_prompt) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 */
    setPrompt(page, 5, SP::prompt);
    /* First 2 cols are prompt */
    fillSC(page, 5, 0, 2, '$', SC::prompt);
    /* Next 2 are input */
    fillSC(page, 5, 2, 4, 'l', SC::input);
    /* Rest is output */
    fillSC(page, 5, 4, 10, 'o', SC::output);
    /* Row 6 has output then prompt starts */
    fillSC(page, 6, 0, 3, 'o', SC::output);
    /* Next prompt marker on same row */
    fillSC(page, 6, 3, 6, '$', SC::prompt);
    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting output should stop before prompt/input */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::output).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 4, 5));
    ASSERT_TRUE(screenAt(*s, hl.end, 2, 6));
}

TEST(page_list, PageList_highlightSemanticContent_output_to_end_of_screen) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Single prompt on row 15, no following prompt */
    setPrompt(page, 15, SP::prompt);
    fillSC(page, 15, 0, 2, '$', SC::prompt);
    fillSC(page, 15, 2, 4, 'c', SC::input);
    fillSC(page, 15, 4, 10, 'o', SC::output);
    /* Row 16 has output then prompt to bound it */
    fillSC(page, 16, 0, 8, 'o', SC::output);
    markSC(page, 16, 8, 10, SC::prompt);

    /* Highlighting output with no following prompt */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 15, SC::output).value;
    ASSERT_TRUE(screenAt(*s, hl.start, 4, 15));
    ASSERT_TRUE(screenAt(*s, hl.end, 7, 16));
}

TEST(page_list, PageList_highlightSemanticContent_output_no_output_returns_null) {
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 with only prompt and input, no output */
    setPrompt(page, 5, SP::prompt);
    /* First 3 cols are prompt */
    fillSC(page, 5, 0, 3, '$', SC::prompt);
    /* Rest is input (must explicitly mark all cells to avoid default .output) */
    fillSC(page, 5, 3, 10, 'c', SC::input);
    /* Mark rows 6-9 as input to ensure no output between prompts */
    for (size_t y = 6; y < 10; y++) markSC(page, y, 0, 10, SC::input);
    /* Prompt on row 10 (no output between prompts) */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting output should return null when there's no output */
    ASSERT_FALSE(hlAt(*s, 0, 5, SC::output).has);
}

TEST(page_list, PageList_highlightSemanticContent_output_skips_empty_cells) {
    /* Tests that empty cells with default .output semantic content are
     * not selected as output. This can happen when a prompt/input line
     * doesn't fill the entire row - trailing cells have default .output. */
    ListHolder s(opts(10, 20, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Prompt on row 5 - only fills first 3 cells, rest are empty with default .output */
    setPrompt(page, 5, SP::prompt);
    /* First 3 cols are prompt with text */
    fillSC(page, 5, 0, 3, '$', SC::prompt);
    /* Cells 3-9 are empty (codepoint = 0) with default .output semantic content
     * This simulates what happens when a short prompt is written */

    /* Row 6 has input (short, doesn't fill line) */
    fillSC(page, 6, 0, 4, 'l', SC::input);
    /* Cells 4-9 are empty with default .output */

    /* Row 7-8 have actual output with text */
    for (size_t y = 7; y < 9; y++) fillSC(page, y, 0, 5, 'o', SC::output);

    /* Prompt on row 10 */
    setPrompt(page, 10, SP::prompt);

    /* Highlighting output should skip empty cells on rows 5-6 and find
     * the actual output starting at row 7 */
    const PageList::HighlightUntracked hl = hlAt(*s, 0, 5, SC::output).value;
    /* Output should start at row 7, not row 5 (where empty cells have default .output) */
    ASSERT_TRUE(screenAt(*s, hl.start, 0, 7));
    ASSERT_TRUE(screenAt(*s, hl.end, 4, 8));
}

/* Wisp: upstream's "grow so we take up at least 5 pages" loop. */
static void growFivePages(PageList &s) {
    Page *page = s.pages.last->page();
    growPagesPaused(s, (size_t)page->capacity.rows * 5);
}

static bool pinAtNode(const Pin *p, Node *node, size_t y, size_t x) { return p->node == node && p->y == y && p->x == x; }

TEST(page_list, PageList_erase) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow so we take up at least 5 pages. */
    growFivePages(*s);
    ASSERT_TRUE(6 == s->totalPages());

    /* Our total rows should be large */
    ASSERT_TRUE(s->total_rows > s->rows);

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseHistory(Maybe<Point>());
    ASSERT_TRUE(s->rows == s->total_rows);

    /* We should be back to just one page */
    ASSERT_TRUE(1 == s->totalPages());
    ASSERT_TRUE(s->pages.first == s->pages.last);
}

TEST(page_list, PageList_erase_reaccounts_page_size) {
    ListHolder s(opts(80, 24));
    const size_t start_size = s->page_size;

    /* Grow so we take up at least 5 pages. */
    growFivePages(*s);
    ASSERT_TRUE(s->page_size > start_size);

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseHistory(Maybe<Point>());
    ASSERT_TRUE(start_size == s->page_size);
}

TEST(page_list, PageList_erase_row_with_tracked_pin_resets_to_top_left) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up at least 5 pages. */
    growFivePages(*s);

    /* Our total rows should be large */
    ASSERT_TRUE(s->total_rows > s->rows);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::history()).value);

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseHistory(Maybe<Point>());
    ASSERT_TRUE(s->rows == s->total_rows);

    /* Our pin should move to the first page */
    ASSERT_TRUE(pinAtNode(p, s->pages.first, 0, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_erase_row_with_tracked_pin_shifts) {
    ListHolder s(opts(80, 24));

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(2, 4)).value);

    /* Erase only a few rows in our active */
    s->eraseActive(3);
    ASSERT_TRUE(s->rows == s->total_rows);

    /* Our pin should move to the first page */
    ASSERT_TRUE(pinAtNode(p, s->pages.first, 0, 2));
    s->untrackPin(p);
}

TEST(page_list, PageList_erase_row_with_tracked_pin_is_erased) {
    ListHolder s(opts(80, 24));

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(2, 2)).value);

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseActive(3);
    ASSERT_TRUE(s->rows == s->total_rows);

    /* Our pin should move to the first page */
    ASSERT_TRUE(pinAtNode(p, s->pages.first, 0, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_erase_resets_viewport_to_active_if_moves_within_active) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up at least 5 pages. */
    growFivePages(*s);

    /* Move our viewport to the top */
    s->scroll(Scroll::deltaRow(-(ptrdiff_t)s->total_rows));
    ASSERT_TRUE(s->viewport == Viewport::top);

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseHistory(Maybe<Point>());
    ASSERT_TRUE(s->viewport == Viewport::active);
}

TEST(page_list, PageList_erase_resets_viewport_if_inside_erased_page_but_not_active) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up at least 5 pages. */
    growFivePages(*s);

    /* Move our viewport to the top */
    s->scroll(Scroll::deltaRow(-(ptrdiff_t)s->total_rows));
    ASSERT_TRUE(s->viewport == Viewport::top);

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseHistory(Point::history(0, 2));
    ASSERT_TRUE(s->viewport == Viewport::top);
}

TEST(page_list, PageList_erase_resets_viewport_to_active_if_top_is_inside_active) {
    ListHolder s(opts(80, 24));

    /* Grow so we take up at least 5 pages. */
    growFivePages(*s);

    /* Move our viewport to the top */
    s->scroll(Scroll::top());

    /* Erase the entire history, we should be back to just our active set. */
    s->eraseHistory(Maybe<Point>());
    ASSERT_TRUE(s->viewport == Viewport::active);
}

TEST(page_list, PageList_erase_active_regrows_automatically) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->totalRows() == s->rows);
    s->eraseActive(10);
    ASSERT_TRUE(s->totalRows() == s->rows);
}

TEST(page_list, PageList_erase_a_one_row_active) {
    ListHolder s(opts(10, 1));
    ASSERT_TRUE(1 == s->totalPages());

    /* Write our letter */
    Page *page = s->pages.first->page();
    for (size_t y = 0; y < s->rows; y++) *page->getRowAndCell(0, y).cell = page::Cell::init('A');

    s->eraseActive(0);
    ASSERT_TRUE(s->rows == s->total_rows);

    /* The row should be empty */
    ASSERT_TRUE(0 == s->getCell(Point::active(0, 0)).value.cell->contentCodepoint());
}

TEST(page_list, PageList_eraseRowBounded_less_than_full_row) {
    ListHolder s(opts(80, 10));

    /* Pins */
    Pin *p_top = s->trackPin(s->pin(Point::active(0, 5)).value);
    Pin *p_bot = s->trackPin(s->pin(Point::active(0, 8)).value);
    Pin *p_out = s->trackPin(s->pin(Point::active(0, 9)).value);

    /* Erase only a few rows in our active */
    s->eraseRowBounded(Point::active(0, 5), 3);
    ASSERT_TRUE(s->rows == s->totalRows());

    /* The erased rows should be dirty */
    ASSERT_TRUE(s->isDirty(Point::active(0, 5)));
    ASSERT_TRUE(s->isDirty(Point::active(0, 6)));
    ASSERT_TRUE(s->isDirty(Point::active(0, 7)));

    ASSERT_TRUE(pinAtNode(p_top, s->pages.first, 4, 0));
    ASSERT_TRUE(pinAtNode(p_bot, s->pages.first, 7, 0));
    ASSERT_TRUE(pinAtNode(p_out, s->pages.first, 9, 0));
    s->untrackPin(p_out);
    s->untrackPin(p_bot);
    s->untrackPin(p_top);
}

TEST(page_list, PageList_eraseRowBounded_with_pin_at_top) {
    ListHolder s(opts(80, 10));

    /* Pins */
    Pin *p_top = s->trackPin(s->pin(Point::active(5, 0)).value);

    /* Erase only a few rows in our active */
    s->eraseRowBounded(Point::active(0, 0), 3);
    ASSERT_TRUE(s->rows == s->totalRows());

    /* The erased rows should be dirty */
    ASSERT_TRUE(s->isDirty(Point::active(0, 0)));
    ASSERT_TRUE(s->isDirty(Point::active(0, 1)));
    ASSERT_TRUE(s->isDirty(Point::active(0, 2)));

    ASSERT_TRUE(pinAtNode(p_top, s->pages.first, 0, 0));
    s->untrackPin(p_top);
}

TEST(page_list, PageList_eraseRowBounded_full_rows_single_page) {
    ListHolder s(opts(80, 10));

    /* Pins */
    Pin *p_in = s->trackPin(s->pin(Point::active(0, 7)).value);
    Pin *p_out = s->trackPin(s->pin(Point::active(0, 9)).value);

    /* Erase only a few rows in our active */
    s->eraseRowBounded(Point::active(0, 5), 10);
    ASSERT_TRUE(s->rows == s->totalRows());

    /* The erased rows should be dirty */
    for (uint32_t y = 5; y < 10; y++) ASSERT_TRUE(s->isDirty(Point::active(0, y)));

    /* Our pin should move to the first page */
    ASSERT_TRUE(pinAtNode(p_in, s->pages.first, 6, 0));
    ASSERT_TRUE(pinAtNode(p_out, s->pages.first, 8, 0));
    s->untrackPin(p_out);
    s->untrackPin(p_in);
}

/* Wisp: "Grow to two pages so our active area straddles". */
static bool growToStraddle(PageList &s) {
    Page *page = s.pages.last->page();
    page->pauseIntegrityChecks(true);
    const size_t n = (size_t)page->capacity.rows - page->size.rows;
    for (size_t i = 0; i < n; i++) (void)growNode(&s);
    page->pauseIntegrityChecks(false);
    if (!s.growRows(5)) return false;
    return 2 == s.totalPages() && 5 == s.pages.last->rows();
}

TEST(page_list, PageList_eraseRowBounded_full_rows_two_pages) {
    ListHolder s(opts(80, 10));

    /* Grow to two pages so our active area straddles */
    ASSERT_TRUE(growToStraddle(*s));

    /* Pins */
    Pin *p_first = s->trackPin(s->pin(Point::active(0, 4)).value);
    Pin *p_first_out = s->trackPin(s->pin(Point::active(0, 3)).value);
    Pin *p_in = s->trackPin(s->pin(Point::active(0, 8)).value);
    Pin *p_out = s->trackPin(s->pin(Point::active(0, 9)).value);

    {
        ASSERT_TRUE(pinAtNode(p_first, s->pages.last->prev, p_first->node->rows() - 1, 0));
        ASSERT_TRUE(pinAtNode(p_first_out, s->pages.last->prev, p_first_out->node->rows() - 2, 0));
        ASSERT_TRUE(pinAtNode(p_in, s->pages.last, 3, 0));
        ASSERT_TRUE(pinAtNode(p_out, s->pages.last, 4, 0));
    }

    /* Erase only a few rows in our active */
    s->eraseRowBounded(Point::active(0, 4), 4);

    /* The erased rows should be dirty */
    for (uint32_t y = 4; y < 8; y++) ASSERT_TRUE(s->isDirty(Point::active(0, y)));

    /* In page in first page is shifted */
    ASSERT_TRUE(pinAtNode(p_first, s->pages.last->prev, p_first->node->rows() - 2, 0));

    /* Out page in first page should not be shifted */
    ASSERT_TRUE(pinAtNode(p_first_out, s->pages.last->prev, p_first_out->node->rows() - 2, 0));

    /* In page is shifted */
    ASSERT_TRUE(pinAtNode(p_in, s->pages.last, 2, 0));

    /* Out page is not shifted */
    ASSERT_TRUE(pinAtNode(p_out, s->pages.last, 4, 0));
    s->untrackPin(p_out);
    s->untrackPin(p_in);
    s->untrackPin(p_first_out);
    s->untrackPin(p_first);
}

static hyperlink::Hyperlink implicitLink(const char *uri, uint32_t implicit) {
    hyperlink::Hyperlink l;
    l.uri = (const uint8_t *)uri;
    l.uri_len = strlen(uri);
    l.id = hyperlink::Hyperlink::Id::makeImplicit(implicit);
    return l;
}

/* Shared setup of the two "hyperlink-dense row crosses page boundary" tests. */
static bool denseHyperlinkSetup(PageList &s, size_t link_count) {
    /* Grow to two pages so our active area straddles them: the first
     * page is exactly full and the second page holds the last 5 rows
     * of the active area. */
    if (!growToStraddle(s)) return false;

    /* Mark each active row with a codepoint so we can verify the
     * shift afterwards. Row y gets codepoint '0' + y at x = 0. */
    for (uint32_t y = 0; y < 10; y++) {
        const Pin row_pin = s.pin(Point::active(0, y)).value;
        *row_pin.rowAndCell().cell = page::Cell::init('0' + y);
    }

    /* Fill the top row of the second page (active y=5) with more
     * unique hyperlinks ('A' through 'J') than the first page's
     * default hyperlink capacity can hold. We must increase the
     * second page's capacity to even create such a row; the first
     * page keeps its default capacity. */
    while (s.pages.last->page()->hyperlink_set.layout.cap <= link_count) {
        (void)incCap(s, s.pages.last, IncCap::hyperlink_bytes);
    }
    if (!(s.pages.first->page()->hyperlink_set.layout.cap < link_count)) return false;
    {
        Page *page = s.pages.last->page();
        for (size_t x = 0; x < link_count; x++) {
            char buf[64];
            snprintf(buf, sizeof buf, "http://example.com/%zu", x);
            hyperlink::Id id;
            if (page->insertHyperlink(implicitLink(buf, (uint32_t)x), &id) != page::PageError::none) return false;
            const Page::RowAndCell rac = page->getRowAndCell(x, 0);
            *rac.cell = page::Cell::init((uint32_t)('A' + x));
            if (page->setHyperlink(rac.row, rac.cell, id) != page::PageError::none) return false;
            page->hyperlink_set.use((const void *)page->memory, id);
        }
    }
    return true;
}

static bool denseRowIntact(PageList &s, size_t link_count) {
    /* Every cell of the dense row must still resolve to a real
     * hyperlink entry with the correct URI. A half-applied erase
     * leaves cells whose hyperlink flag is set but that have no map
     * entry, which aborts in clearCells later. */
    for (size_t x = 0; x < link_count; x++) {
        const PageList::Cell list_cell = s.getCell(Point::active((size::CellCountInt)x, 4)).value;
        if (!list_cell.cell->hyperlink()) return false;
        Page *page = list_cell.node->page();
        hyperlink::Id id;
        if (!page->lookupHyperlink(list_cell.cell, &id)) return false;
        const hyperlink::PageEntry *link = page->hyperlink_set.get((const void *)page->memory, id);
        char buf[64];
        snprintf(buf, sizeof buf, "http://example.com/%zu", x);
        if (link->uri.len != strlen(buf)) return false;
        if (memcmp(link->uri.slice((const void *)page->memory), buf, link->uri.len) != 0) return false;
    }

    /* All pages must pass integrity checks. */
    for (Node *node = s.pages.first; node; node = node->next) node->page()->assertIntegrity();
    return true;
}

TEST(page_list, PageList_eraseRow_hyperlink_dense_row_crosses_page_boundary) {
    /* Regression test: when eraseRow shifts rows up across a page
     * boundary, the top row of the next page is cloned into the last
     * row of the previous page. If the previous page doesn't have
     * enough capacity for the managed memory of that row (hyperlinks,
     * styles, etc.) the error propagated out AFTER the previous page
     * had already been rotated and its tracked pins moved, leaving
     * the page list half-mutated.
     *
     * eraseRow must instead increase the destination page's capacity
     * and retry, the same way insertLines/deleteLines and
     * cursorScrollAbove handle their cross-page copies. */
    ListHolder s(opts(80, 10));
    const size_t link_count = 10;
    ASSERT_TRUE(denseHyperlinkSetup(*s, link_count));

    /* Track a pin in the shifted region of the first page to verify
     * it survives the capacity change of its node. */
    Pin *p = s->trackPin(s->pin(Point::active(3, 1)).value);

    /* Erase the first active row. The dense hyperlink row must cross
     * the page boundary into the first page, which requires growing
     * the first page's hyperlink capacity. */
    s->eraseRow(Point::active(0, 0));

    /* Every remaining row shifted up by one: the '0' marker row was
     * erased, the dense row moved up across the page boundary to
     * row 4, and the last row was cleared. */
    const uint32_t expected[10] = {'1', '2', '3', '4', 'A', '6', '7', '8', '9', 0};
    for (uint32_t y = 0; y < 10; y++) {
        ASSERT_TRUE(expected[y] == s->getCell(Point::active(0, y)).value.cell->contentCodepoint());
    }

    ASSERT_TRUE(denseRowIntact(*s, link_count));

    /* Our tracked pin shifted up by one row and still points into
     * the (possibly replaced) first page. */
    ASSERT_TRUE(s->pages.first == p->node);
    const Point p_pt = s->pointFromPin(point::Tag::active, *p).value;
    ASSERT_TRUE(3 == p_pt.c.x);
    ASSERT_TRUE(0 == p_pt.c.y);
    s->untrackPin(p);
}

TEST(page_list, PageList_eraseRowBounded_hyperlink_dense_row_crosses_page_boundary) {
    /* Same as the eraseRow variant above but for eraseRowBounded,
     * which has the same rotate-then-clone structure and had the
     * same bug: a cross-page row clone failure propagated out after
     * the first page had already been rotated. */
    ListHolder s(opts(80, 10));
    const size_t link_count = 10;
    ASSERT_TRUE(denseHyperlinkSetup(*s, link_count));

    /* Erase the first active row with a limit that extends into the
     * second page (5 rows remain in the first page, so a limit of 6
     * forces the cross-page path). The dense hyperlink row must cross
     * the page boundary into the first page. */
    s->eraseRowBounded(Point::active(0, 0), 6);

    /* Rows within the limit shifted up by one: the '0' marker row was
     * erased, the dense row moved up across the page boundary to
     * row 4, row 6 is the new blank row, and rows past the limit are
     * unchanged. */
    const uint32_t expected[10] = {'1', '2', '3', '4', 'A', '6', 0, '7', '8', '9'};
    for (uint32_t y = 0; y < 10; y++) {
        ASSERT_TRUE(expected[y] == s->getCell(Point::active(0, y)).value.cell->contentCodepoint());
    }

    ASSERT_TRUE(denseRowIntact(*s, link_count));
}

TEST(page_list, PageList_clone) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), PageList::Clone(Point::screen()), &s2) == page::PageError::none);
    ASSERT_TRUE((size_t)s->rows == s2.totalRows());
    s2.deinit();
}

TEST(page_list, PageList_clone_partial_trimmed_right) {
    ListHolder s(opts(80, 20));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());
    ASSERT_TRUE(s->growRows(30));

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), PageList::Clone(Point::screen(), Point::screen(0, 39)), &s2) ==
                page::PageError::none);
    ASSERT_TRUE(40 == s2.totalRows());
    s2.deinit();
}

TEST(page_list, PageList_clone_partial_trimmed_left) {
    ListHolder s(opts(80, 20));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());
    ASSERT_TRUE(s->growRows(30));

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), PageList::Clone(Point::screen(0, 10)), &s2) == page::PageError::none);
    ASSERT_TRUE(40 == s2.totalRows());
    s2.deinit();
}

static page::Cell bgRed() {
    page::Cell c;
    c.setContentTag(page::Cell::ContentTag::bg_color_rgb);
    page::Cell::RGB rgb;
    rgb.r = 0xFF;
    rgb.g = 0;
    rgb.b = 0;
    c.setContentColorRgb(rgb);
    return c;
}

static bool activeAt(PageList &s, const Pin *p, uint32_t x, uint32_t y) {
    return pointEq(s.pointFromPin(point::Tag::active, *p), Point::active((size::CellCountInt)x, y));
}

static bool activeScreenIs(PageList &s, uint32_t y) {
    return cellScreenPoint(s, Point::active()).eql(Point::screen(0, y));
}

static PageList::Clone cloneOpts(const Point &top, Maybe<Point> bot = Maybe<Point>(),
                                 PageList::Clone::TrackedPinsRemap *remap = nullptr) {
    return PageList::Clone(top, bot, remap);
}

TEST(page_list, PageList_clone_partial_trimmed_left_reclaims_styles) {
    ListHolder s(opts(80, 20));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());
    ASSERT_TRUE(s->growRows(30));

    /* Style the rows we're trimming */
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        style::Style st;
        st.flags.bold = true;
        style::Id style_id;
        ASSERT_TRUE(page->styles.add((const void *)page->memory, st, &style_id) == ref_counted_set::AddError::none);

        PageList::RowIterator it = s->rowIterator(Dir::left_up, Point::screen(), Point::screen(0, 9));
        Pin p;
        while (it.next(&p)) {
            const Page::RowAndCell rac = p.rowAndCell();
            rac.row->setStyled(true);
            page::Cell c = page::Cell::init('A');
            c.setStyleId(style_id);
            *rac.cell = c;
            page->styles.use((const void *)page->memory, style_id);
        }

        /* We're over-counted by 1 because `add` implies `use`. */
        page->styles.release((const void *)page->memory, style_id);

        /* Expect to have one style */
        ASSERT_TRUE(1 == page->styles.count());
    }

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::screen(0, 10)), &s2) == page::PageError::none);
    ASSERT_TRUE(40 == s2.totalRows());

    {
        ASSERT_TRUE(s2.pages.first == s2.pages.last);
        Page *page = s2.pages.first->page();
        ASSERT_TRUE(0 == page->styles.count());
    }
    s2.deinit();
}

TEST(page_list, PageList_clone_partial_trimmed_both) {
    ListHolder s(opts(80, 20));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());
    ASSERT_TRUE(s->growRows(30));

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::screen(0, 10), Point::screen(0, 35)), &s2) ==
                page::PageError::none);
    ASSERT_TRUE(26 == s2.totalRows());
    s2.deinit();
}

TEST(page_list, PageList_clone_less_than_active) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::active(0, 5)), &s2) == page::PageError::none);
    ASSERT_TRUE((size_t)s->rows == s2.totalRows());
    s2.deinit();
}

TEST(page_list, PageList_clone_remap_tracked_pin) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Put a tracked pin in the screen */
    Pin *p = s->trackPin(s->pin(Point::active(0, 6)).value);

    PageList::Clone::TrackedPinsRemap pin_remap;
    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::active(0, 5), Maybe<Point>(), &pin_remap), &s2) ==
                page::PageError::none);

    /* We should be able to find our tracked pin */
    ASSERT_TRUE(pin_remap.count(p) == 1);
    Pin *p2 = pin_remap[p];
    ASSERT_TRUE(pointEq(s2.pointFromPin(point::Tag::active, *p2), Point::active(0, 1)));
    s2.deinit();
    s->untrackPin(p);
}

TEST(page_list, PageList_clone_remap_tracked_pin_not_in_cloned_area) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Put a tracked pin in the screen */
    Pin *p = s->trackPin(s->pin(Point::active(0, 3)).value);

    PageList::Clone::TrackedPinsRemap pin_remap;
    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::active(0, 5), Maybe<Point>(), &pin_remap), &s2) ==
                page::PageError::none);

    /* We should be able to find our tracked pin */
    ASSERT_TRUE(pin_remap.count(p) == 0);
    s2.deinit();
    s->untrackPin(p);
}

TEST(page_list, PageList_clone_full_dirty) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Mark a row as dirty */
    s->markDirty(Point::active(0, 0));
    s->markDirty(Point::active(0, 12));
    s->markDirty(Point::active(0, 23));

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::screen()), &s2) == page::PageError::none);
    ASSERT_TRUE((size_t)s->rows == s2.totalRows());

    /* Should still be dirty */
    ASSERT_TRUE(s2.isDirty(Point::active(0, 0)));
    ASSERT_FALSE(s2.isDirty(Point::active(0, 1)));
    ASSERT_TRUE(s2.isDirty(Point::active(0, 12)));
    ASSERT_FALSE(s2.isDirty(Point::active(0, 14)));
    ASSERT_TRUE(s2.isDirty(Point::active(0, 23)));
    s2.deinit();
}

TEST(page_list, PageList_resize_no_reflow_more_rows) {
    ListHolder s(opts(10, 3, (size_t)0));
    ASSERT_TRUE(3 == s->totalRows());

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, 2)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 10, false)));
    ASSERT_TRUE(10 == s->rows);
    ASSERT_TRUE(10 == s->totalRows());

    /* Our cursor should not move because we have no scrollback so
     * we just grew. */
    ASSERT_TRUE(activeAt(*s, p, 0, 2));

    ASSERT_TRUE(activeScreenIs(*s, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_no_reflow_more_rows_with_history) {
    ListHolder s(opts(10, 3));
    ASSERT_TRUE(s->growRows(50));
    ASSERT_TRUE(activeScreenIs(*s, 50));

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, 2)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 5, false)));
    ASSERT_TRUE(5 == s->rows);
    ASSERT_TRUE(53 == s->totalRows());

    /* Our cursor should move since it's in the scrollback */
    ASSERT_TRUE(activeAt(*s, p, 0, 4));

    ASSERT_TRUE(activeScreenIs(*s, 48));
    s->untrackPin(p);
}

static void writeRowsA(PageList &s, Page *page) {
    /* Write into all rows so we don't get trim behavior */
    for (size_t y = 0; y < s.rows; y++) *page->getRowAndCell(0, y).cell = page::Cell::init('A');
}

static void writeRowsY(PageList &s, Page *page) {
    for (size_t y = 0; y < s.rows; y++) *page->getRowAndCell(0, y).cell = page::Cell::init((uint32_t)y);
}

static uint32_t cpAtPin(PageList &s, const Pin *p) {
    const Point cursor = s.pointFromPin(point::Tag::active, *p).value;
    return s.getCell(Point::active(cursor.c.x, cursor.c.y)).value.cell->contentCodepoint();
}

TEST(page_list, PageList_resize_no_reflow_less_rows) {
    ListHolder s(opts(10, 10, (size_t)0));
    ASSERT_TRUE(10 == s->totalRows());

    /* This is required for our writing below to work */
    ASSERT_TRUE(s->pages.first == s->pages.last);
    writeRowsA(*s, s->pages.first->page());

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 5, false)));
    ASSERT_TRUE(5 == s->rows);
    ASSERT_TRUE(10 == s->totalRows());
    ASSERT_TRUE(activeScreenIs(*s, 5));
}

TEST(page_list, PageList_resize_no_reflow_one_rows) {
    ListHolder s(opts(10, 10, (size_t)0));
    ASSERT_TRUE(10 == s->totalRows());

    /* This is required for our writing below to work */
    ASSERT_TRUE(s->pages.first == s->pages.last);
    writeRowsA(*s, s->pages.first->page());

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 1, false)));
    ASSERT_TRUE(1 == s->rows);
    ASSERT_TRUE(10 == s->totalRows());
    ASSERT_TRUE(activeScreenIs(*s, 9));
}

TEST(page_list, PageList_resize_no_reflow_less_rows_cursor_on_bottom) {
    ListHolder s(opts(10, 10, (size_t)0));
    ASSERT_TRUE(10 == s->totalRows());

    /* This is required for our writing below to work */
    ASSERT_TRUE(s->pages.first == s->pages.last);
    writeRowsY(*s, s->pages.first->page());

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, 9)).value);
    ASSERT_TRUE(9 == cpAtPin(*s, p));

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 5, false)));
    ASSERT_TRUE(5 == s->rows);
    ASSERT_TRUE(10 == s->totalRows());

    /* Our cursor should move since it's in the scrollback */
    ASSERT_TRUE(activeAt(*s, p, 0, 4));

    ASSERT_TRUE(activeScreenIs(*s, 5));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_no_reflow_less_rows_cursor_in_scrollback) {
    ListHolder s(opts(10, 10, (size_t)0));
    ASSERT_TRUE(10 == s->totalRows());

    /* This is required for our writing below to work */
    ASSERT_TRUE(s->pages.first == s->pages.last);
    writeRowsY(*s, s->pages.first->page());

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, 2)).value);
    ASSERT_TRUE(2 == cpAtPin(*s, p));

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 5, false)));
    ASSERT_TRUE(5 == s->rows);
    ASSERT_TRUE(10 == s->totalRows());

    /* Our cursor should move since it's in the scrollback */
    ASSERT_FALSE(s->pointFromPin(point::Tag::active, *p).has);
    ASSERT_TRUE(pointEq(s->pointFromPin(point::Tag::screen, *p), Point::screen(0, 2)));

    ASSERT_TRUE(activeScreenIs(*s, 5));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_no_reflow_less_rows_trims_blank_lines) {
    ListHolder s(opts(10, 5, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Write codepoint into first line */
    *page->getRowAndCell(0, 0).cell = page::Cell::init('A');

    /* Fill remaining lines with a background color */
    for (size_t y = 1; y < s->rows; y++) *page->getRowAndCell(0, y).cell = bgRed();

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, 0)).value);
    ASSERT_TRUE('A' == cpAtPin(*s, p));

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 2, false)));
    ASSERT_TRUE(2 == s->rows);
    ASSERT_TRUE(2 == s->totalRows());

    /* Our cursor should not move since we trimmed */
    ASSERT_TRUE(activeAt(*s, p, 0, 0));

    ASSERT_TRUE(activeScreenIs(*s, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_no_reflow_less_rows_trims_blank_lines_cursor_in_blank_line) {
    ListHolder s(opts(10, 5, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Write codepoint into first line */
    *page->getRowAndCell(0, 0).cell = page::Cell::init('A');

    /* Fill remaining lines with a background color */
    for (size_t y = 1; y < s->rows; y++) *page->getRowAndCell(0, y).cell = bgRed();

    /* Put a tracked pin in a blank line */
    Pin *p = s->trackPin(s->pin(Point::active(0, 3)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 2, false)));
    ASSERT_TRUE(2 == s->rows);
    ASSERT_TRUE(4 == s->totalRows());

    /* Our cursor should not move since we trimmed */
    ASSERT_TRUE(activeAt(*s, p, 0, 1));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_no_reflow_less_rows_trims_blank_lines_erases_pages) {
    ListHolder s(opts(100, 5, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Resize to take up two pages */
    {
        const int rows = page->capacity.rows + 10;
        ASSERT_TRUE(s->resize(rz(-1, rows, false)));
        ASSERT_TRUE(2 == s->totalPages());
    }

    /* Write codepoint into first line */
    *page->getRowAndCell(0, 0).cell = page::Cell::init('A');

    /* Resize down. Every row except the first is blank so we
     * should erase the second page. */
    ASSERT_TRUE(s->resize(rz(-1, 5, false)));
    ASSERT_TRUE(5 == s->rows);
    ASSERT_TRUE(5 == s->totalRows());
    ASSERT_TRUE(1 == s->totalPages());
}

TEST(page_list, PageList_resize_no_reflow_more_rows_extends_blank_lines) {
    ListHolder s(opts(10, 3, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();

    /* Write codepoint into first line */
    *page->getRowAndCell(0, 0).cell = page::Cell::init('A');

    /* Fill remaining lines with a background color */
    for (size_t y = 1; y < s->rows; y++) *page->getRowAndCell(0, y).cell = bgRed();

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 7, false)));
    ASSERT_TRUE(7 == s->rows);
    ASSERT_TRUE(7 == s->totalRows());
    ASSERT_TRUE(activeScreenIs(*s, 0));
}

TEST(page_list, PageList_resize_no_reflow_more_rows_contains_viewport) {
    /* When the rows are increased we need to make sure that the viewport
     * doesn't end up below the active area if it's currently in pin mode. */

    ListHolder s(opts(5, 5, (size_t)1));
    ASSERT_TRUE(s->pages.first == s->pages.last);

    /* Make it so we have scrollback */
    (void)growNode(&*s);

    ASSERT_TRUE(5 == s->rows);
    ASSERT_TRUE(6 == s->totalRows());

    /* Set viewport above active by scrolling up one. */
    s->scroll(Scroll::deltaRow(-1));
    /* The viewport should be a pin now. */
    ASSERT_TRUE(Viewport::top == s->viewport);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(-1, 7, false)));
    ASSERT_TRUE(7 == s->rows);
    ASSERT_TRUE(7 == s->totalRows());

    /* Question: maybe the viewport should actually be in the active
     * here and not pinned to the top. */
    ASSERT_TRUE(Viewport::top == s->viewport);
}

/* Wisp: `page.getCells(row).len` is the page's column count. */
static bool allRowsHaveCols(PageList &s, size_t cols) {
    PageList::RowIterator it = s.rowIterator(Dir::right_down, Point::screen());
    Pin offset;
    while (it.next(&offset)) {
        (void)offset.rowAndCell();
        if (offset.node->page()->size.cols != cols) return false;
    }
    return true;
}

static PageList::Resize rzc(int cols, int rows, bool reflow, size::CellCountInt cx, size::CellCountInt cy,
                            Pin *pin = nullptr) {
    PageList::Resize r = rz(cols, rows, reflow);
    r.cursor = PageList::Resize::Cursor(cx, cy, pin);
    return r;
}

static page::Cell wideCell(uint32_t cp, page::Cell::Wide w) {
    page::Cell c = page::Cell::init(cp);
    c.setWide(w);
    return c;
}

TEST(page_list, PageList_resize_no_reflow_less_cols) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Resize */
    ASSERT_TRUE(s->resize(rz(5, -1, false)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(10 == s->totalRows());

    ASSERT_TRUE(allRowsHaveCols(*s, 5));
}

TEST(page_list, PageList_resize_no_reflow_less_cols_pin_in_trimmed_cols) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(8, 2)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(5, -1, false)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(10 == s->totalRows());

    ASSERT_TRUE(allRowsHaveCols(*s, 5));

    ASSERT_TRUE(activeAt(*s, p, 4, 2));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_no_reflow_less_cols_clears_graphemes) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Add a grapheme. */
    Page *page = s->pages.first->page();
    {
        const Page::RowAndCell rac = page->getRowAndCell(9, 0);
        *rac.cell = page::Cell::init('A');
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 'A') == page::PageError::none);
    }
    ASSERT_TRUE(1 == page->graphemeCount());

    /* Resize */
    ASSERT_TRUE(s->resize(rz(5, -1, false)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(10 == s->totalRows());

    PageList::PageIterator it = s->pageIterator(Dir::right_down, Point::screen());
    PageList::Chunk chunk;
    while (it.next(&chunk)) {
        ASSERT_TRUE(0 == chunk.node->page()->graphemeCount());
    }
}

TEST(page_list, PageList_resize_no_reflow_more_cols) {
    ListHolder s(opts(5, 3, (size_t)0));

    /* Resize */
    ASSERT_TRUE(s->resize(rz(10, -1, false)));
    ASSERT_TRUE(10 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    ASSERT_TRUE(allRowsHaveCols(*s, 10));
}

TEST(page_list, PageList_resize_no_reflow_more_cols_with_spacer_head) {
    typedef page::Cell::Wide Wide;
    ListHolder s(opts(2, 3, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            rac.row->setWrap(true);
            *rac.cell = page::Cell::init('x');
        }
        *page->getRowAndCell(1, 0).cell = wideCell(0, Wide::spacer_head);
        *page->getRowAndCell(0, 1).cell = wideCell(0x1F600, Wide::wide);
        *page->getRowAndCell(1, 1).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(3, -1, false)));
    ASSERT_TRUE(3 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            ASSERT_TRUE('x' == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
            /* try testing.expect(!rac.row.wrap); */
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(1, 0);
            ASSERT_TRUE(0 == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(2, 0);
            ASSERT_TRUE(0 == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
        }
    }
}

/* Regression test for fuzz crash. When we shrink cols and then
 * grow back, the page retains capacity from the original size so the grow
 * takes the fast path (just bumps page.size.cols). If any row has a
 * spacer_head at the old last column, that cell is no longer at the end
 * of the wider row, violating page integrity. */
TEST(page_list, PageList_resize_no_reflow_grow_cols_fast_path_with_spacer_head) {
    typedef page::Cell::Wide Wide;
    ListHolder s(opts(10, 3, (size_t)0));

    /* Shrink to 5 cols. The page keeps capacity for 10 cols. */
    ASSERT_TRUE(s->resize(rz(5, -1, false)));
    ASSERT_TRUE(5 == s->cols);

    /* Place a spacer_head at the last column (col 4) on two rows
     * to simulate a wide character that didn't fit at the right edge. */
    {
        Page *page = s->pages.first->page();

        /* Row 0: 'x' at col 0..3, spacer_head at col 4, wrap = true */
        *page->getRowAndCell(0, 0).cell = page::Cell::init('x');
        {
            const Page::RowAndCell rac = page->getRowAndCell(4, 0);
            *rac.cell = wideCell(0, Wide::spacer_head);
            rac.row->setWrap(true);
        }

        /* Row 1: spacer_head at col 4, wrap = true */
        {
            const Page::RowAndCell rac = page->getRowAndCell(4, 1);
            *rac.cell = wideCell(0, Wide::spacer_head);
            rac.row->setWrap(true);
        }
    }

    /* Grow back to 10 cols. This must not leave stale spacer_head
     * cells at col 4 (which is no longer the last column). */
    ASSERT_TRUE(s->resize(rz(10, -1, false)));
    ASSERT_TRUE(10 == s->cols);

    /* Verify the old spacer_head positions are now narrow. */
    {
        Page *page = s->pages.first->page();
        {
            const Page::RowAndCell rac = page->getRowAndCell(4, 0);
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
            ASSERT_FALSE(rac.row->wrap());
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(4, 1);
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
            ASSERT_FALSE(rac.row->wrap());
        }
    }
}

/* This test is a bit convoluted so I want to explain: what we are trying
 * to verify here is that when we increase cols such that our rows per page
 * shrinks, we don't fragment our rows across many pages because this ends
 * up wasting a lot of memory.
 *
 * This is particularly important for alternate screen buffers where we
 * don't have scrollback so our max size is very small. If we don't do this,
 * we end up pruning our pages and that causes resizes to fail! */
TEST(page_list, PageList_resize_no_reflow_more_cols_forces_less_rows_per_page) {
    /* This test requires initially that our rows fit into one page. */
    const size::CellCountInt cols = 5;
    const size::CellCountInt rows = 150;
    ASSERT_TRUE(stdAdjust(cols).rows >= rows);
    ListHolder s(opts(cols, rows, (size_t)0));

    /* Then we need to resize our cols so that our rows per page shrinks.
     * This will force our resize to split our rows across two pages. */
    {
        size::CellCountInt new_cols = 50;
        Capacity cap = stdAdjust(new_cols);
        while (cap.rows >= rows) {
            new_cols += 50;
            cap = stdAdjust(new_cols);
        }
        ASSERT_TRUE(s->resize(rz(new_cols, -1, false)));
        ASSERT_TRUE(new_cols == s->cols);
        ASSERT_TRUE(rows == s->totalRows());
    }

    /* Every page except the last should be full */
    for (Node *page = s->pages.first; page; page = page->next) {
        if (page == s->pages.last) break;
        ASSERT_TRUE(page->capacity().rows == page->rows());
    }

    /* Now we need to resize again to a col size that further shrinks
     * our last capacity. */
    {
        Page *page = s->pages.first->page();
        ASSERT_TRUE(page->size.rows == page->capacity.rows);
        size::CellCountInt new_cols = (size::CellCountInt)(page->size.cols + 50);
        Capacity cap = stdAdjust(new_cols);
        while (cap.rows >= page->size.rows) {
            new_cols += 50;
            cap = stdAdjust(new_cols);
        }

        ASSERT_TRUE(s->resize(rz(new_cols, -1, false)));
        ASSERT_TRUE(new_cols == s->cols);
        ASSERT_TRUE(rows == s->totalRows());
    }

    /* Every page except the last should be full */
    for (Node *page = s->pages.first; page; page = page->next) {
        if (page == s->pages.last) break;
        ASSERT_TRUE(page->capacity().rows == page->rows());
    }
}

TEST(page_list, PageList_resize_no_reflow_less_cols_then_more_cols) {
    ListHolder s(opts(5, 3, (size_t)0));

    /* Resize less */
    ASSERT_TRUE(s->resize(rz(2, -1, false)));
    ASSERT_TRUE(2 == s->cols);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(5, -1, false)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    ASSERT_TRUE(allRowsHaveCols(*s, 5));
}

TEST(page_list, PageList_resize_no_reflow_less_rows_and_cols) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Resize less */
    ASSERT_TRUE(s->resize(rz(5, 7, false)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(7 == s->rows);

    ASSERT_TRUE(allRowsHaveCols(*s, 5));
}

TEST(page_list, PageList_resize_less_rows_and_cols_cursor_at_bottom) {
    ListHolder s(opts(80, 24, (size_t)0));

    Pin *cursor_pin = s->trackPin(s->pin(Point::active(0, (uint32_t)(s->rows - 1))).value);

    /* Shrink both axes such that the original cursor.y is strictly past the
     * new row count, so resizeWithoutReflow leaves self.rows < c.y + 1. */
    ASSERT_TRUE(s->resize(rzc(79, 20, true, 0, 23, cursor_pin)));
    ASSERT_TRUE(79 == s->cols);
    ASSERT_TRUE(20 == s->rows);

    /* remaining_rows saturates to 0, so the cursor lands on the new bottom row. */
    ASSERT_TRUE(activeAt(*s, cursor_pin, 0, (uint32_t)(s->rows - 1)));
    s->untrackPin(cursor_pin);
}

TEST(page_list, PageList_resize_less_rows_and_cols_cursor_near_top_pushed_to_scrollback) {
    ListHolder s(opts(80, 24));

    /* Fill every active row with non-blank content so that shrinking rows
     * can't trim trailing blank lines and instead pushes the top rows into
     * scrollback. */
    {
        PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
        Pin p;
        while (it.next(&p)) {
            const Page::RowAndCell rac = p.rowAndCell();
            page::Cell *cells = p.node->page()->getCells(rac.row);
            const size_t n = p.node->page()->size.cols;
            for (size_t x = 0; x < n; x++) cells[x] = page::Cell::init((uint32_t)('A' + (x % 26)));
        }
    }

    /* Cursor near the top of the active area. After we shrink rows the active
     * area top moves down past this pin, so it ends up in scrollback. */
    Pin *cursor_pin = s->trackPin(s->pin(Point::active(0, 0)).value);

    /* Shrink both axes with reflow. resizeWithoutReflow shrinks self.rows
     * first, leaving the cursor pin above the new active area, then resizeCols
     * walks .left_up from the cursor pin toward the active-area top. */
    ASSERT_TRUE(s->resize(rzc(79, 20, true, 0, 0, cursor_pin)));
    ASSERT_TRUE(79 == s->cols);
    ASSERT_TRUE(20 == s->rows);

    /* The active area is anchored to the bottom, so shrinking rows pushed the
     * top-of-screen cursor into scrollback: it no longer resolves to an
     * active-area coordinate, but it remains a valid screen pin. */
    ASSERT_FALSE(s->pointFromPin(point::Tag::active, *cursor_pin).has);
    ASSERT_TRUE(s->pointFromPin(point::Tag::screen, *cursor_pin).has);

    /* Integrity must hold after the resize. */
    s->assertIntegrity();
    s->untrackPin(cursor_pin);
}

TEST(page_list, PageList_resize_no_reflow_more_rows_and_less_cols) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Resize less */
    ASSERT_TRUE(s->resize(rz(5, 20, false)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(20 == s->rows);
    ASSERT_TRUE(20 == s->totalRows());

    ASSERT_TRUE(allRowsHaveCols(*s, 5));
}

TEST(page_list, PageList_resize_more_rows_and_cols_doesnt_fit_in_single_std_page) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Resize to a size that requires more than one page to fit our rows. */
    const int new_cols = 600;
    const int new_rows = 600;
    const Capacity cap = stdAdjust(new_cols);
    ASSERT_TRUE(cap.rows < new_rows);

    ASSERT_TRUE(s->resize(rz(new_cols, new_rows, true)));
    ASSERT_TRUE(new_cols == s->cols);
    ASSERT_TRUE(new_rows == s->rows);
    ASSERT_TRUE((size_t)new_rows == s->totalRows());
}

TEST(page_list, PageList_resize_no_reflow_empty_screen) {
    ListHolder s(opts(5, 5, (size_t)0));

    /* Resize */
    ASSERT_TRUE(s->resize(rz(10, 10, false)));
    ASSERT_TRUE(10 == s->cols);
    ASSERT_TRUE(10 == s->rows);
    ASSERT_TRUE(10 == s->totalRows());

    ASSERT_TRUE(allRowsHaveCols(*s, 10));
}

TEST(page_list, PageList_resize_no_reflow_more_cols_forces_smaller_cap) {
    /* We want a cap that forces us to have less rows */
    const Capacity cap = stdAdjust(100);
    const Capacity cap2 = stdAdjust(500);
    ASSERT_TRUE(500 == cap2.cols);
    ASSERT_TRUE(cap2.rows < cap.rows);

    /* Create initial cap, fits in one page */
    ListHolder s(opts(cap.cols, cap.rows));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    for (size_t y = 0; y < s->rows; y++) {
        for (size_t x = 0; x < s->cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init('A');
    }

    /* Resize to our large cap */
    const size_t rows = s->totalRows();
    ASSERT_TRUE(s->resize(rz(cap2.cols, -1, false)));

    /* Our total rows should be the same, and contents should be the same. */
    ASSERT_TRUE(rows == s->totalRows());
    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::screen());
    Pin offset;
    while (it.next(&offset)) {
        const Page::RowAndCell rac = offset.rowAndCell();
        const page::Cell *cells = offset.node->page()->getCells(rac.row);
        ASSERT_TRUE(cap2.cols == offset.node->page()->size.cols);
        ASSERT_TRUE('A' == cells[0].contentCodepoint());
    }
}

/* Wisp: `s.pin(.{ .active = .{ .y = y } }).?` row and cells. */
static page::Row *activeRow(PageList &s, uint32_t y) { return s.pin(Point::active(0, y)).value.rowAndCell().row; }
static page::Cell *activeCells(PageList &s, uint32_t y) {
    size_t len;
    return s.pin(Point::active(0, y)).value.cells(Pin::CellSubset::all, &len);
}

/* Wisp: rows alternate wrap / wrap_continuation, every cell 'A'. */
static void wrapPairsA(PageList &s, Page *page) {
    for (size_t y = 0; y < s.rows; y++) {
        const Page::RowAndCell rac = page->getRowAndCell(0, y);
        if (y % 2 == 0) {
            rac.row->setWrap(true);
        } else {
            rac.row->setWrapContinuation(true);
        }
        for (size_t x = 0; x < s.cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init('A');
    }
}

/* Wisp: set row y wrap/continuation flag and cells to their x index. */
static void rowX(PageList &s, Page *page, size_t y, bool wrap, bool cont) {
    const Page::RowAndCell rac = page->getRowAndCell(0, y);
    if (wrap) rac.row->setWrap(true);
    if (cont) rac.row->setWrapContinuation(true);
    for (size_t x = 0; x < s.cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init((uint32_t)x);
}

/* Wisp: "Grow to the capacity of the first page" then growRows(n). */
static bool growFirstPageThen(PageList &s, size_t n) {
    Page *page = s.pages.first->page();
    page->pauseIntegrityChecks(true);
    for (size_t i = page->size.rows; i < page->capacity.rows; i++) (void)growNode(&s);
    page->pauseIntegrityChecks(false);
    if (1 != s.totalPages()) return false;
    if (!s.growRows(n)) return false;
    return 2 == s.totalPages();
}

TEST(page_list, PageList_resize_no_reflow_more_rows_adds_blank_rows_if_cursor_at_bottom) {
    ListHolder s(opts(5, 3));

    /* Grow to 5 total rows, simulating 3 active + 2 scrollback */
    ASSERT_TRUE(s->growRows(2));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    for (size_t y = 0; y < s->totalRows(); y++) *page->getRowAndCell(0, y).cell = page::Cell::init((uint32_t)y);

    /* Active should be on row 3 */
    ASSERT_TRUE(activeScreenIs(*s, 2));

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, (uint32_t)(s->rows - 2))).value);
    const Point original_cursor = s->pointFromPin(point::Tag::active, *p).value;
    ASSERT_TRUE(3 == s->getCell(Point::active(original_cursor.c.x, original_cursor.c.y)).value.cell->contentCodepoint());

    /* Resize */
    ASSERT_TRUE(s->resizeWithoutReflow(rzc(-1, 10, false, 0, (size::CellCountInt)(s->rows - 2))));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(10 == s->rows);

    /* Our cursor should not change */
    ASSERT_TRUE(original_cursor.eql(s->pointFromPin(point::Tag::active, *p).value));

    /* 12 because we have our 10 rows in the active + 2 in the scrollback
     * because we're preserving the cursor. */
    ASSERT_TRUE(12 == s->totalRows());

    /* Active should be at the same place it was. */
    ASSERT_TRUE(activeScreenIs(*s, 2));

    /* Go through our active, we should get only 3,4,5 */
    for (uint32_t y = 0; y < 3; y++) {
        ASSERT_TRUE(y + 2 == s->getCell(Point::active(0, y)).value.cell->contentCodepoint());
    }
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_more_cols_no_wrapped_rows) {
    ListHolder s(opts(5, 3, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    for (size_t y = 0; y < s->rows; y++) {
        for (size_t x = 0; x < s->cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init('A');
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(10, -1, true)));
    ASSERT_TRUE(10 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::screen());
    Pin offset;
    while (it.next(&offset)) {
        const Page::RowAndCell rac = offset.rowAndCell();
        const page::Cell *cells = offset.node->page()->getCells(rac.row);
        ASSERT_TRUE(10 == offset.node->page()->size.cols);
        ASSERT_TRUE('A' == cells[0].contentCodepoint());
    }
}

TEST(page_list, PageList_resize_reflow_more_cols_wrapped_rows) {
    ListHolder s(opts(2, 4, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    wrapPairsA(*s, s->pages.first->page());

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Active should still be on top */
    ASSERT_TRUE(activeScreenIs(*s, 0));

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::screen());
    {
        /* First row should be unwrapped */
        Pin offset;
        ASSERT_TRUE(it.next(&offset));
        const Page::RowAndCell rac = offset.rowAndCell();
        const page::Cell *cells = offset.node->page()->getCells(rac.row);
        ASSERT_FALSE(rac.row->wrap());
        ASSERT_TRUE(4 == offset.node->page()->size.cols);
        ASSERT_TRUE('A' == cells[0].contentCodepoint());
        ASSERT_TRUE('A' == cells[2].contentCodepoint());
    }
}

TEST(page_list, PageList_resize_reflow_invalidates_viewport_offset_cache) {
    ListHolder s(opts(2, 4));
    ASSERT_TRUE(s->growRows(20));

    wrapPairsA(*s, s->pages.last->page());

    /* Scroll to a pinned viewport in history */
    const size_t pin_y = 10;
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, (uint32_t)pin_y)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, pin_y, s->rows));

    /* Resize with reflow - unwrapping rows changes total_rows */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);

    /* Verify scrollbar cache was invalidated during reflow */
    ASSERT_TRUE(sbEq(s->scrollbar(), s->total_rows, 5, s->rows));
}

TEST(page_list, PageList_resize_reflow_more_cols_creates_multiple_pages) {
    /* We want a wide viewport so our row limit is rather small. This will
     * force the reflow below to create multiple pages, which we assert. */
    Capacity cap;
    for (size::CellCountInt current = 100;; current += 100) {
        cap = stdAdjust(current);
        if (cap.rows < 100) break;
    }

    ListHolder s(opts(cap.cols, cap.rows));

    /* Wrap every other row so every line is wrapped for reflow */
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        for (size_t y = 0; y < s->rows; y++) {
            const Page::RowAndCell rac = page->getRowAndCell(0, y);
            if (y % 2 == 0) {
                rac.row->setWrap(true);
            } else {
                rac.row->setWrapContinuation(true);
            }
            *rac.cell = page::Cell::init('A');
        }
    }

    /* Resize */
    Capacity newcap;
    ASSERT_TRUE(cap.adjust(Capacity::Adjustment::withCols((size::CellCountInt)(cap.cols + 100)), &newcap));
    ASSERT_TRUE(newcap.rows < cap.rows);
    ASSERT_TRUE(s->resize(rz(newcap.cols, -1, true)));
    ASSERT_TRUE(newcap.cols == s->cols);
    ASSERT_TRUE(cap.rows == s->totalRows());

    {
        size_t count = 0;
        for (Node *page = s->pages.first; page; page = page->next) {
            count += 1;

            /* All pages should have the new capacity */
            ASSERT_TRUE(newcap.cols == page->capacity().cols);
            ASSERT_TRUE(newcap.rows == page->capacity().rows);
        }

        /* We should have more than one page, meaning we created at least
         * one page. This is the critical aspect of this test so if this
         * ever goes false we need to adjust this test. */
        ASSERT_TRUE(count > 1);
    }
}

TEST(page_list, PageList_resize_reflow_more_cols_wrap_across_page_boundary) {
    ListHolder s(opts(2, 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page. */
    ASSERT_TRUE(growFirstPageThen(*s, 1));

    /* At this point, we have some rows on the first page, and some on the second.
     * We can now wrap across the boundary condition. */
    {
        Page *page = s->pages.first->page();
        rowX(*s, page, page->size.rows - 1, true, false);
    }
    rowX(*s, s->pages.last->page(), 0, false, true);

    /* We expect one fewer rows since we unwrapped a row. */
    const size_t end_rows = s->totalRows() - 1;

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(end_rows == s->totalRows());

    {
        /* PAGE 1 ROW 6280, ACTIVE 8 */
        const page::Row *row = activeRow(*s, 8);
        ASSERT_FALSE(row->wrap());
        ASSERT_FALSE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 8);
        for (int i = 0; i < 4; i++) ASSERT_FALSE(cells[i].hasText());
    }
    {
        /* PAGE 1 ROW 6281, ACTIVE 9 */
        const page::Row *row = activeRow(*s, 9);
        ASSERT_FALSE(row->wrap());
        ASSERT_FALSE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 9);
        ASSERT_TRUE(0 == cells[0].contentCodepoint());
        ASSERT_TRUE(1 == cells[1].contentCodepoint());
        ASSERT_TRUE(0 == cells[2].contentCodepoint());
        ASSERT_TRUE(1 == cells[3].contentCodepoint());
    }
}

TEST(page_list, PageList_resize_reflow_more_cols_wrap_across_page_boundary_cursor_in_second_page) {
    ListHolder s(opts(2, 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page. */
    ASSERT_TRUE(growFirstPageThen(*s, 1));

    /* At this point, we have some rows on the first page, and some on the second.
     * We can now wrap across the boundary condition. */
    {
        Page *page = s->pages.first->page();
        rowX(*s, page, page->size.rows - 1, true, false);
    }
    rowX(*s, s->pages.last->page(), 0, false, true);

    /* Put a tracked pin in wrapped row on the last page */
    Pin *p = s->trackPin(s->pin(Point::active(1, 9)).value);
    ASSERT_TRUE(p->node == s->pages.last);

    /* We expect one fewer rows since we unwrapped a row. */
    const size_t end_rows = s->totalRows() - 1;

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(end_rows == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 3, 9));

    {
        const page::Row *row = activeRow(*s, 9);
        ASSERT_FALSE(row->wrap());

        const page::Cell *cells = activeCells(*s, 9);
        ASSERT_TRUE(0 == cells[0].contentCodepoint());
        ASSERT_TRUE(1 == cells[1].contentCodepoint());
        ASSERT_TRUE(0 == cells[2].contentCodepoint());
        ASSERT_TRUE(1 == cells[3].contentCodepoint());
    }
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_wrap_across_page_boundary_cursor_in_second_page) {
    ListHolder s(opts(5, 10));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page. */
    ASSERT_TRUE(growFirstPageThen(*s, 5));

    /* At this point, we have some rows on the first page, and some on the second.
     * We can now wrap across the boundary condition. */
    {
        Page *page = s->pages.first->page();
        rowX(*s, page, page->size.rows - 1, true, false);
    }
    rowX(*s, s->pages.last->page(), 0, false, true);

    /* Put a tracked pin in wrapped row on the last page */
    Pin *p = s->trackPin(s->pin(Point::active(2, 5)).value);
    ASSERT_TRUE(p->node == s->pages.last);
    ASSERT_TRUE(p->y == 0);

    /* Resize */
    ASSERT_TRUE(s->resize(rzc(4, -1, true, 2, 5)));
    ASSERT_TRUE(4 == s->cols);

    /* Our cursor should remain on the same cell */
    ASSERT_TRUE(activeAt(*s, p, 3, 5));

    {
        /* PAGE 0 ROW 7895, ACTIVE 3 */
        const page::Row *row = activeRow(*s, 3);
        ASSERT_FALSE(row->wrap());
        ASSERT_FALSE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 3);
        for (int i = 0; i < 4; i++) ASSERT_FALSE(cells[i].hasText());
    }
    {
        /* PAGE 0 ROW 7896, ACTIVE 4 */
        const page::Row *row = activeRow(*s, 4);
        ASSERT_TRUE(row->wrap());
        ASSERT_FALSE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 4);
        ASSERT_TRUE(0 == cells[0].contentCodepoint());
        ASSERT_TRUE(1 == cells[1].contentCodepoint());
        ASSERT_TRUE(2 == cells[2].contentCodepoint());
        ASSERT_TRUE(3 == cells[3].contentCodepoint());
    }
    {
        /* PAGE 0 ROW 7897, ACTIVE 5 */
        const page::Row *row = activeRow(*s, 5);
        ASSERT_TRUE(row->wrap());
        ASSERT_TRUE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 5);
        ASSERT_TRUE(4 == cells[0].contentCodepoint());
        ASSERT_TRUE(0 == cells[1].contentCodepoint());
        ASSERT_TRUE(1 == cells[2].contentCodepoint());
        ASSERT_TRUE(2 == cells[3].contentCodepoint());
    }
    {
        /* PAGE 0 ROW 7898, ACTIVE 6 */
        const page::Row *row = activeRow(*s, 6);
        ASSERT_FALSE(row->wrap());
        ASSERT_TRUE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 6);
        ASSERT_TRUE(3 == cells[0].contentCodepoint());
        ASSERT_TRUE(4 == cells[1].contentCodepoint());
    }
    {
        /* PAGE 0 ROW 7899, ACTIVE 7 */
        const page::Row *row = activeRow(*s, 7);
        ASSERT_FALSE(row->wrap());
        ASSERT_FALSE(row->wrap_continuation());

        const page::Cell *cells = activeCells(*s, 7);
        for (int i = 0; i < 4; i++) ASSERT_FALSE(cells[i].hasText());
    }
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_more_cols_cursor_in_wrapped_row) {
    ListHolder s(opts(2, 4, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    rowX(*s, page, 0, true, false);
    rowX(*s, page, 1, false, true);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(1, 1)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 3, 0));
    s->untrackPin(p);
}

static hyperlink::Hyperlink explicitLink(const std::string &uri, const std::string &id) {
    hyperlink::Hyperlink l;
    l.uri = (const uint8_t *)uri.data();
    l.uri_len = uri.size();
    l.id = hyperlink::Hyperlink::Id::makeExplicit((const uint8_t *)id.data(), id.size());
    return l;
}
static hyperlink::Hyperlink implicitLinkS(const std::string &uri, uint32_t implicit) {
    hyperlink::Hyperlink l;
    l.uri = (const uint8_t *)uri.data();
    l.uri_len = uri.size();
    l.id = hyperlink::Hyperlink::Id::makeImplicit(implicit);
    return l;
}

TEST(page_list, PageList_resize_reflow_more_cols_cursor_in_not_wrapped_row) {
    ListHolder s(opts(2, 4, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    rowX(*s, page, 0, true, false);
    rowX(*s, page, 1, false, true);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(1, 0)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 1, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_more_cols_cursor_in_wrapped_row_that_isnt_unwrapped) {
    ListHolder s(opts(2, 4, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    rowX(*s, page, 0, true, false);
    rowX(*s, page, 1, true, true);
    rowX(*s, page, 2, false, true);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(1, 2)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 1, 1));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_more_cols_no_reflow_preserves_semantic_prompt) {
    ListHolder s(opts(2, 4, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        setPrompt(s->pages.first->page(), 1, SP::prompt);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        ASSERT_TRUE(page->getRowAndCell(0, 1).row->semantic_prompt() == SP::prompt);
    }
}

/* Wisp: "Grow to the capacity of the first page and add one more row so
 * that we have two pages total." */
static bool twoPages(PageList &s) {
    if (!growFirstPageThen(s, 1)) return false;
    return s.pages.first != s.pages.last && s.pages.last == s.pages.first->next;
}

TEST(page_list, PageList_resize_reflow_exceeds_hyperlink_memory_forcing_capacity_increase) {
    ListHolder s(opts(2, 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page and add
     * one more row so that we have two pages total. */
    ASSERT_TRUE(twoPages(*s));

    /* We use almost all string alloc capacity with a hyperlink in the final
     * row of the first page, and do the same on the first row of the second
     * page. We also mark the row as wrapped so that when we resize with more
     * cols the row unwraps and we have a single row that requires almost two
     * times the base string alloc capacity.
     *
     * This forces the reflow to increase capacity. */
    const std::string big(page::string_bytes_default - 1, 'a');
    const std::string small(26, 'A');

    /* Almost hit string alloc cap in bottom right of first page.
     * Mark the final row as wrapped. */
    {
        Page *page = s->pages.first->page();
        hyperlink::Id id;
        ASSERT_TRUE(page->insertHyperlink(implicitLinkS(big, 0), &id) == page::PageError::none);
        const Page::RowAndCell rac = page->getRowAndCell(page->size.cols - 1, page->size.rows - 1);
        rac.row->setWrap(true);
        *rac.cell = page::Cell::init('X');
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == page::PageError::none);
        hyperlink::Id id2;
        ASSERT_TRUE(page->insertHyperlink(implicitLinkS(small, 1), &id2) == page::PageError::StringsOutOfMemory);
    }

    /* Almost hit string alloc cap in top left of second page.
     * Mark the first row as a wrap continuation. */
    {
        Page *page = s->pages.last->page();
        hyperlink::Id id;
        ASSERT_TRUE(page->insertHyperlink(implicitLinkS(big, 1), &id) == page::PageError::none);
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        rac.row->setWrapContinuation(true);
        *rac.cell = page::Cell::init('X');
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == page::PageError::none);
        hyperlink::Id id2;
        ASSERT_TRUE(page->insertHyperlink(implicitLinkS(small, 2), &id2) == page::PageError::StringsOutOfMemory);
    }

    /* Resize to 1 column wider, unwrapping the row. */
    ASSERT_TRUE(s->resize(rz(s->cols + 1, -1, true)));
}

TEST(page_list, PageList_resize_reflow_hyperlink_dupe_string_alloc_chunk_rounding) {
    ListHolder s(opts(2, 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page and add
     * one more row so that we have two pages total. */
    ASSERT_TRUE(twoPages(*s));

    /* The string allocator hands out 32-byte chunks and every allocation
     * is rounded up to the chunk size independently. Duping a hyperlink
     * during reflow allocates the URI and the explicit ID separately, so
     * two separate allocations can require one more chunk than a single
     * combined allocation of the same total byte length.
     *
     * We arrange for the reflow target page to have exactly two free
     * chunks (64 bytes) remaining when a hyperlink with a 33-byte URI
     * (2 chunks) and a 31-byte explicit ID (1 chunk) is reflowed into
     * it. The combined byte length (64 bytes -> 2 chunks) fits, but the
     * separate allocations (3 chunks) do not, so the reflow must grow
     * the string capacity rather than panic or drop the hyperlink.
     *
     * The two hyperlinked cells are joined as a single wrapped row so
     * that they are always reflowed into the same target page. */

    const std::string uri_a(page::string_bytes_default - 64, 'a');
    const std::string uri_b(33, 'b');
    const std::string id_b(31, 'i');

    /* Hyperlink A in the bottom right of the first page. Mark the final
     * row as wrapped. */
    {
        Page *page = s->pages.first->page();
        hyperlink::Id id;
        ASSERT_TRUE(page->insertHyperlink(implicitLinkS(uri_a, 0), &id) == page::PageError::none);
        const Page::RowAndCell rac = page->getRowAndCell(page->size.cols - 1, page->size.rows - 1);
        rac.row->setWrap(true);
        *rac.cell = page::Cell::init('A');
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == page::PageError::none);

        /* Sanity check the chunk math: the remaining 64 bytes fit as a
         * single allocation but not as the two separate allocations that
         * inserting (or duping) hyperlink B performs. */
        uint8_t *buf;
        ASSERT_TRUE(page->string_alloc.alloc<uint8_t>((const void *)page->memory, 64, &buf));
        page->string_alloc.free((const void *)page->memory, buf, 64);
        hyperlink::Id id2;
        ASSERT_TRUE(page->insertHyperlink(explicitLink(uri_b, id_b), &id2) == page::PageError::StringsOutOfMemory);
    }

    /* Hyperlink B in the top left of the second page. Mark the first
     * row as a wrap continuation. */
    {
        Page *page = s->pages.last->page();
        hyperlink::Id id;
        ASSERT_TRUE(page->insertHyperlink(explicitLink(uri_b, id_b), &id) == page::PageError::none);
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        rac.row->setWrapContinuation(true);
        *rac.cell = page::Cell::init('B');
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == page::PageError::none);
    }

    /* Resize to 1 column wider, unwrapping the row. */
    ASSERT_TRUE(s->resize(rz(s->cols + 1, -1, true)));

    /* Both hyperlinks must have survived the reflow intact. */
    size_t found = 0;
    for (Node *node = s->pages.first; node; node = node->next) {
        Page *page = node->page();
        for (size_t y = 0; y < page->size.rows; y++) {
            for (size_t x = 0; x < page->size.cols; x++) {
                const Page::RowAndCell rac = page->getRowAndCell(x, y);
                if (!rac.cell->hyperlink()) continue;
                found += 1;

                hyperlink::Id link_id;
                ASSERT_TRUE(page->lookupHyperlink(rac.cell, &link_id));
                const hyperlink::PageEntry *entry = page->hyperlink_set.get((const void *)page->memory, link_id);
                const std::string uri((const char *)entry->uri.slice((const void *)page->memory), entry->uri.len);
                switch (entry->id.tag) {
                case hyperlink::PageEntry::Id::Tag::implicit: ASSERT_TRUE(uri_a == uri); break;
                case hyperlink::PageEntry::Id::Tag::explicit_: {
                    ASSERT_TRUE(uri_b == uri);
                    const std::string idv((const char *)entry->id.explicit_.slice((const void *)page->memory),
                                          entry->id.explicit_.len);
                    ASSERT_TRUE(id_b == idv);
                    break;
                }
                }
            }
        }
    }
    ASSERT_TRUE(2 == found);
}

TEST(page_list, PageList_resize_reflow_exceeds_grapheme_memory_forcing_capacity_increase) {
    ListHolder s(opts(4, 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page and add
     * one more row so that we have two pages total. */
    ASSERT_TRUE(twoPages(*s));

    /* We use all grapheme alloc capacity with four maximum-sized graphemes on
     * each page. The two rows form one wrapped logical line across the page
     * boundary, so resizing wider moves all eight graphemes into one page and
     * requires almost two times the base grapheme alloc capacity.
     *
     * This forces the reflow to increase capacity. */

    uint32_t suffixes[page::grapheme_max_len];
    for (size_t i = 0; i < page::grapheme_max_len; i++) suffixes[i] = 'a';

    /* Fill the final row of the first page and mark it as wrapped. */
    {
        Page *page = s->pages.first->page();
        const size_t y = page->size.rows - 1;
        page->getRow(y)->setWrap(true);

        for (size_t x = 0; x < page->size.cols; x++) {
            const Page::RowAndCell rac = page->getRowAndCell(x, y);
            *rac.cell = page::Cell::init('X');
            ASSERT_TRUE(page->setGraphemes(rac.row, rac.cell, suffixes, page::grapheme_max_len) ==
                        page::PageError::none);
        }
        ASSERT_TRUE(page->grapheme_alloc.capacityBytes() == page->grapheme_alloc.usedBytes((const void *)page->memory));
        uint32_t *tmp;
        ASSERT_FALSE(page->grapheme_alloc.alloc<uint32_t>((const void *)page->memory, 16, &tmp));
    }

    /* Fill the first row of the second page and mark it as a continuation. */
    {
        Page *page = s->pages.last->page();
        page->getRow(0)->setWrapContinuation(true);

        for (size_t x = 0; x < page->size.cols; x++) {
            const Page::RowAndCell rac = page->getRowAndCell(x, 0);
            *rac.cell = page::Cell::init('X');
            ASSERT_TRUE(page->setGraphemes(rac.row, rac.cell, suffixes, page::grapheme_max_len) ==
                        page::PageError::none);
        }
        uint32_t *tmp;
        ASSERT_FALSE(page->grapheme_alloc.alloc<uint32_t>((const void *)page->memory, 16, &tmp));
    }

    /* Resize to 1 column wider, unwrapping the row. */
    ASSERT_TRUE(s->resize(rz(s->cols + 1, -1, true)));
}

TEST(page_list, PageList_resize_reflow_exceeds_style_memory_forcing_capacity_increase) {
    ListHolder s(opts((size::CellCountInt)(page::std_capacity().styles - 1), 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Grow to the capacity of the first page and add
     * one more row so that we have two pages total. */
    ASSERT_TRUE(twoPages(*s));

    /* Give each cell in the final row of the first page a unique style.
     * Mark the final row as wrapped. */
    {
        Page *page = s->pages.first->page();
        for (size_t x = 0; x < s->cols; x++) {
            style::Style st;
            style::RGB rgb;
            rgb.r = (uint8_t)x;
            rgb.g = (uint8_t)(x >> 8);
            rgb.b = (uint8_t)(x >> 16);
            st.bg_color = style::Style::Color::makeRgb(rgb);
            style::Id id;
            if (page->styles.add((const void *)page->memory, st, &id) != ref_counted_set::AddError::none) break;

            const Page::RowAndCell rac = page->getRowAndCell(x, page->size.rows - 1);
            rac.row->setWrap(true);
            rac.row->setStyled(true);
            page::Cell c = page::Cell::init('X');
            c.setStyleId(id);
            *rac.cell = c;
        }
    }

    /* Do the same for the first row of the second page.
     * Mark the first row as a wrap continuation. */
    {
        Page *page = s->pages.last->page();
        for (size_t x = 0; x < s->cols; x++) {
            style::Style st;
            style::RGB rgb;
            rgb.r = (uint8_t)x;
            rgb.g = (uint8_t)(x >> 8);
            rgb.b = (uint8_t)(x >> 16);
            st.fg_color = style::Style::Color::makeRgb(rgb);
            style::Id id;
            if (page->styles.add((const void *)page->memory, st, &id) != ref_counted_set::AddError::none) break;

            const Page::RowAndCell rac = page->getRowAndCell(x, 0);
            rac.row->setWrapContinuation(true);
            rac.row->setStyled(true);
            page::Cell c = page::Cell::init('X');
            c.setStyleId(id);
            *rac.cell = c;
        }
    }

    /* Resize to twice as wide, fully unwrapping the row. */
    ASSERT_TRUE(s->resize(rz(s->cols * 2, -1, true)));
}

TEST(page_list, PageList_resize_reflow_more_cols_unwrap_wide_spacer_head) {
    typedef page::Cell::Wide Wide;
    ListHolder s(opts(2, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            rac.row->setWrap(true);
            *rac.cell = page::Cell::init('x');
        }
        *page->getRowAndCell(1, 0).cell = wideCell(0, Wide::spacer_head);
        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 1);
            rac.row->setWrapContinuation(true);
            *rac.cell = wideCell(0x1F600, Wide::wide);
        }
        *page->getRowAndCell(1, 1).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            ASSERT_TRUE('x' == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
            ASSERT_FALSE(rac.row->wrap());
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(1, 0);
            ASSERT_TRUE(0x1F600 == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::wide == rac.cell->wide());
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(2, 0);
            ASSERT_TRUE(0 == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::spacer_tail == rac.cell->wide());
        }
    }
}

typedef page::Cell::Wide Wide;

static bool cellIs(Page *page, size_t x, size_t y, uint32_t cp, Wide w) {
    const Page::RowAndCell rac = page->getRowAndCell(x, y);
    return rac.cell->contentCodepoint() == cp && rac.cell->wide() == w;
}

/* Wisp: fill `cols` cells of each row with their x index. */
static void fillX(PageList &s, Page *page, size_t cols) {
    for (size_t y = 0; y < s.rows; y++) {
        for (size_t x = 0; x < cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init((uint32_t)x);
    }
}

/* Wisp: next row from the iterator: wrap flag, cols, first codepoint. */
static bool nextRowIs(PageList::RowIterator &it, bool wrap, size_t cols, uint32_t cp0) {
    Pin offset;
    if (!it.next(&offset)) return false;
    const Page::RowAndCell rac = offset.rowAndCell();
    const page::Cell *cells = offset.node->page()->getCells(rac.row);
    return rac.row->wrap() == wrap && offset.node->page()->size.cols == cols && cells[0].contentCodepoint() == cp0;
}

TEST(page_list, PageList_resize_reflow_more_cols_unwrap_wide_spacer_head_across_two_rows) {
    ListHolder s(opts(2, 3, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            rac.row->setWrap(true);
            *rac.cell = page::Cell::init('x');
        }
        *page->getRowAndCell(1, 0).cell = page::Cell::init('x');
        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 1);
            rac.row->setWrapContinuation(true);
            rac.row->setWrap(true);
            *rac.cell = page::Cell::init('x');
        }
        *page->getRowAndCell(1, 1).cell = wideCell(0, Wide::spacer_head);
        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 2);
            rac.row->setWrapContinuation(true);
            *rac.cell = wideCell(0x1F600, Wide::wide);
        }
        *page->getRowAndCell(1, 2).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(cellIs(page, 0, 0, 'x', Wide::narrow));
        ASSERT_TRUE(page->getRowAndCell(0, 0).row->wrap());
        ASSERT_TRUE(cellIs(page, 1, 0, 'x', Wide::narrow));
        ASSERT_TRUE(cellIs(page, 2, 0, 'x', Wide::narrow));
        ASSERT_TRUE(cellIs(page, 3, 0, 0, Wide::spacer_head));
        ASSERT_TRUE(cellIs(page, 0, 1, 0x1F600, Wide::wide));
        ASSERT_TRUE(cellIs(page, 1, 1, 0, Wide::spacer_tail));
    }
}

TEST(page_list, PageList_resize_reflow_more_cols_unwrap_still_requires_wide_spacer_head) {
    ListHolder s(opts(2, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            rac.row->setWrap(true);
            *rac.cell = page::Cell::init('x');
        }
        *page->getRowAndCell(1, 0).cell = page::Cell::init('x');
        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 1);
            rac.row->setWrapContinuation(true);
            *rac.cell = wideCell(0x1F600, Wide::wide);
        }
        *page->getRowAndCell(1, 1).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(3, -1, true)));
    ASSERT_TRUE(3 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(cellIs(page, 0, 0, 'x', Wide::narrow));
        ASSERT_TRUE(page->getRowAndCell(0, 0).row->wrap());
        ASSERT_TRUE(cellIs(page, 1, 0, 'x', Wide::narrow));
        ASSERT_TRUE(cellIs(page, 2, 0, 0, Wide::spacer_head));
        ASSERT_TRUE(cellIs(page, 0, 1, 0x1F600, Wide::wide));
        ASSERT_TRUE(cellIs(page, 1, 1, 0, Wide::spacer_tail));
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_no_reflow_preserves_semantic_prompt) {
    ListHolder s(opts(4, 4, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        setPrompt(page, 1, SP::prompt);
        for (size_t x = 0; x < s->cols; x++) *page->getRowAndCell(x, 1).cell = page::Cell::init((uint32_t)x);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        {
            const page::Row *row = activeRow(*s, 1);
            ASSERT_TRUE(row->wrap());
            ASSERT_TRUE(row->semantic_prompt() == SP::prompt);
        }
        {
            const page::Row *row = activeRow(*s, 2);
            ASSERT_TRUE(row->semantic_prompt() == SP::prompt);
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_no_reflow_preserves_semantic_prompt_on_first_line) {
    ListHolder s(opts(4, 4, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        setPrompt(s->pages.first->page(), 0, SP::prompt);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        ASSERT_TRUE(s->pages.first->page()->getRowAndCell(0, 0).row->semantic_prompt() == SP::prompt);
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_wrap_preserves_semantic_prompt) {
    ListHolder s(opts(4, 4, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        setPrompt(s->pages.first->page(), 0, SP::prompt);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        ASSERT_TRUE(s->pages.first->page()->getRowAndCell(0, 0).row->semantic_prompt() == SP::prompt);
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_no_wrapped_rows) {
    ListHolder s(opts(10, 3, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 4);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(5, -1, true)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::screen());
    Pin offset;
    while (it.next(&offset)) {
        for (size_t x = 0; x < 4; x++) {
            Pin offset_copy = offset;
            offset_copy.x = (size::CellCountInt)x;
            const Page::RowAndCell rac = offset_copy.rowAndCell();
            const page::Cell *cells = offset.node->page()->getCells(rac.row);
            ASSERT_TRUE(5 == offset.node->page()->size.cols);
            ASSERT_TRUE((uint32_t)x == cells[x].contentCodepoint());
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_wrapped_rows) {
    ListHolder s(opts(4, 2));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 4);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Active moves due to scrollback */
    ASSERT_TRUE(activeScreenIs(*s, 2));

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::screen());
    /* First row should be wrapped */
    ASSERT_TRUE(nextRowIs(it, true, 2, 0));
    ASSERT_TRUE(nextRowIs(it, false, 2, 2));
    /* First row should be wrapped */
    ASSERT_TRUE(nextRowIs(it, true, 2, 0));
    ASSERT_TRUE(nextRowIs(it, false, 2, 2));
}

TEST(page_list, PageList_resize_reflow_less_cols_wrapped_rows_with_graphemes) {
    ListHolder s(opts(4, 2));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        for (size_t y = 0; y < s->rows; y++) {
            for (size_t x = 0; x < s->cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init((uint32_t)x);

            const Page::RowAndCell rac = page->getRowAndCell(2, y);
            ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 'A') == page::PageError::none);
        }
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Active moves due to scrollback */
    ASSERT_TRUE(activeScreenIs(*s, 2));

    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::screen());
    for (int k = 0; k < 2; k++) {
        /* First row should be wrapped */
        ASSERT_TRUE(nextRowIs(it, true, 2, 0));
        {
            Pin offset;
            ASSERT_TRUE(it.next(&offset));
            const Page::RowAndCell rac = offset.rowAndCell();
            const page::Cell *cells = offset.node->page()->getCells(rac.row);
            ASSERT_FALSE(rac.row->wrap());
            ASSERT_TRUE(rac.row->grapheme());
            ASSERT_TRUE(2 == offset.node->page()->size.cols);
            ASSERT_TRUE(2 == cells[0].contentCodepoint());

            size_t len = 0;
            const uint32_t *cps = page->lookupGrapheme(rac.cell, &len);
            ASSERT_TRUE(1 == len);
            ASSERT_TRUE('A' == cps[0]);
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_in_wrapped_row) {
    ListHolder s(opts(4, 2));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 4);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(2, 1)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 0, 1));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_wraps_spacer_head) {
    ListHolder s(opts(4, 3, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            rac.row->setWrap(true);
            *rac.cell = page::Cell::init('x');
        }
        *page->getRowAndCell(1, 0).cell = page::Cell::init('x');
        *page->getRowAndCell(2, 0).cell = page::Cell::init('x');
        *page->getRowAndCell(3, 0).cell = wideCell(0, Wide::spacer_head);
        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 1);
            rac.row->setWrapContinuation(true);
            *rac.cell = wideCell(0x1F600, Wide::wide);
        }
        *page->getRowAndCell(1, 1).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(3, -1, true)));
    ASSERT_TRUE(3 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(cellIs(page, 0, 0, 'x', Wide::narrow));
        ASSERT_TRUE(page->getRowAndCell(0, 0).row->wrap());
        ASSERT_TRUE(cellIs(page, 1, 0, 'x', Wide::narrow));
        ASSERT_TRUE(cellIs(page, 2, 0, 'x', Wide::narrow));
        ASSERT_TRUE(cellIs(page, 0, 1, 0x1F600, Wide::wide));
        ASSERT_TRUE(cellIs(page, 1, 1, 0, Wide::spacer_tail));
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_goes_to_scrollback) {
    ListHolder s(opts(4, 2));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 4);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(2, 0)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(4 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_FALSE(s->pointFromPin(point::Tag::active, *p).has);
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_in_unchanged_row) {
    ListHolder s(opts(4, 2));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 2);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(1, 0)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 1, 0));
    s->untrackPin(p);
}

static void cursorBlankSetup(PageList &s) {
    ASSERT_TRUE(s.pages.first == s.pages.last);
    fillX(s, s.pages.first->page(), 2);
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_in_blank_cell) {
    ListHolder s(opts(6, 2));
    cursorBlankSetup(*s);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(2, 0)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    /* Our cursor should not move */
    ASSERT_TRUE(activeAt(*s, p, 2, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_in_final_blank_cell) {
    ListHolder s(opts(6, 2));
    cursorBlankSetup(*s);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(3, 0)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 3, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_in_wrapped_blank_cell) {
    ListHolder s(opts(6, 2));
    cursorBlankSetup(*s);

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(5, 0)).value);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 3, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_blank_lines) {
    ListHolder s(opts(4, 3, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    for (size_t x = 0; x < 4; x++) *page->getRowAndCell(x, 0).cell = page::Cell::init((uint32_t)x);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    /* First row should be wrapped */
    ASSERT_TRUE(nextRowIs(it, true, 2, 0));
    ASSERT_TRUE(nextRowIs(it, false, 2, 2));
}

TEST(page_list, PageList_resize_reflow_less_cols_blank_lines_between) {
    ListHolder s(opts(4, 3, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    for (size_t x = 0; x < 4; x++) *page->getRowAndCell(x, 0).cell = page::Cell::init((uint32_t)x);
    for (size_t x = 0; x < 4; x++) *page->getRowAndCell(x, 2).cell = page::Cell::init((uint32_t)x);

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(5 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    {
        Pin offset;
        ASSERT_TRUE(it.next(&offset));
        ASSERT_FALSE(offset.rowAndCell().row->wrap());
    }
    ASSERT_TRUE(nextRowIs(it, true, 2, 0));
    ASSERT_TRUE(nextRowIs(it, false, 2, 2));
}

TEST(page_list, PageList_resize_reflow_less_cols_blank_lines_between_no_scrollback) {
    ListHolder s(opts(5, 3, (size_t)0));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    Page *page = s->pages.first->page();
    *page->getRowAndCell(0, 0).cell = page::Cell::init('A');
    *page->getRowAndCell(0, 2).cell = page::Cell::init('C');

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(3 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    ASSERT_TRUE(nextRowIs(it, false, 2, 'A'));
    {
        Pin offset;
        ASSERT_TRUE(it.next(&offset));
        const Page::RowAndCell rac = offset.rowAndCell();
        ASSERT_TRUE(0 == offset.node->page()->getCells(rac.row)[0].contentCodepoint());
    }
    ASSERT_TRUE(nextRowIs(it, false, 2, 'C'));
}

TEST(page_list, PageList_resize_reflow_less_cols_cursor_not_on_last_line_preserves_location) {
    ListHolder s(opts(5, 5, (size_t)1));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 2);

    /* Grow blank rows to push our rows back into scrollback */
    ASSERT_TRUE(s->growRows(5));
    ASSERT_TRUE(10 == s->totalRows());

    /* Put a tracked pin in the history */
    Pin *p = s->trackPin(s->pin(Point::active(0, 0)).value);

    /* Resize */
    /* Important: not on last row */
    ASSERT_TRUE(s->resize(rzc(4, -1, true, 1, 1)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(10 == s->totalRows());

    /* Our cursor should move to the first row */
    ASSERT_TRUE(activeAt(*s, p, 0, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_no_scrollback_pull_blank_active) {
    ListHolder s(opts(5, 5, (size_t)1));
    ASSERT_TRUE(s->pages.first == s->pages.last);
    fillX(*s, s->pages.first->page(), 2);

    /* Grow blank rows to push our rows back into scrollback */
    ASSERT_TRUE(s->growRows(5));
    ASSERT_TRUE(10 == s->totalRows());

    Pin *p = s->trackPin(s->pin(Point::active(0, 0)).value);

    /* Resize with no cursor. Normally the trailing blank rows would be
     * trimmed and the active area would slide up over our history. */
    PageList::Resize r = rz(4, -1, true);
    r.pull_scrollback = false;
    ASSERT_TRUE(s->resize(r));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(10 == s->totalRows());

    /* The top of the active area should not move */
    ASSERT_TRUE(activeAt(*s, p, 0, 0));
    s->untrackPin(p);
}

TEST(page_list, PageList_resize_reflow_less_cols_copy_style) {
    ListHolder s(opts(4, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* Create a style */
        style::Style st;
        st.flags.bold = true;
        style::Id style_id;
        ASSERT_TRUE(page->styles.add((const void *)page->memory, st, &style_id) == ref_counted_set::AddError::none);

        for (size_t x = 0; x < (size_t)s->cols - 1; x++) {
            page::Cell c = page::Cell::init((uint32_t)x);
            c.setStyleId(style_id);
            *page->getRowAndCell(x, 0).cell = c;
            page->styles.use((const void *)page->memory, style_id);
        }

        /* We're over-counted by 1 because `add` implies `use`. */
        page->styles.release((const void *)page->memory, style_id);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    Pin offset;
    while (it.next(&offset)) {
        for (size_t x = 0; x < (size_t)s->cols - 1; x++) {
            Pin offset_copy = offset;
            offset_copy.x = (size::CellCountInt)x;
            const Page::RowAndCell rac = offset_copy.rowAndCell();
            const style::Id style_id = rac.cell->style_id();
            ASSERT_TRUE(style_id != 0);

            const style::Style *st =
                offset.node->page()->styles.get((const void *)offset.node->page()->memory, style_id);
            ASSERT_TRUE(st->flags.bold);

            ASSERT_TRUE(rac.row->styled());
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_to_eliminate_a_wide_char) {
    ListHolder s(opts(2, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        *page->getRowAndCell(0, 0).cell = wideCell(0x1F600, Wide::wide);
        *page->getRowAndCell(1, 0).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(1, -1, true)));
    ASSERT_TRUE(1 == s->cols);
    ASSERT_TRUE(1 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        ASSERT_TRUE(cellIs(s->pages.first->page(), 0, 0, 0, Wide::narrow));
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_to_wrap_a_wide_char) {
    ListHolder s(opts(3, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        *page->getRowAndCell(0, 0).cell = page::Cell::init('x');
        *page->getRowAndCell(1, 0).cell = wideCell(0x1F600, Wide::wide);
        *page->getRowAndCell(2, 0).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(cellIs(page, 0, 0, 'x', Wide::narrow));
        ASSERT_TRUE(page->getRowAndCell(0, 0).row->wrap());
        ASSERT_TRUE(cellIs(page, 1, 0, 0, Wide::spacer_head));
        ASSERT_TRUE(cellIs(page, 0, 1, 0x1F600, Wide::wide));
        ASSERT_TRUE(cellIs(page, 1, 1, 0, Wide::spacer_tail));
    }
}

static void widePairs(Page *page, size_t n) {
    for (size_t i = 0; i < n; i++) {
        *page->getRowAndCell(i * 2, 0).cell = wideCell((uint32_t)(0x4E00 + i), Wide::wide);
        *page->getRowAndCell(i * 2 + 1, 0).cell = wideCell(0, Wide::spacer_tail);
    }
}

static bool pairsAt(Page *page, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const size_t y = i / 2;
        const size_t x = (i % 2) * 2;
        if (!cellIs(page, x, y, (uint32_t)(0x4E00 + i), Wide::wide)) return false;
        if (!cellIs(page, x + 1, y, 0, Wide::spacer_tail)) return false;
    }
    return true;
}

TEST(page_list, PageList_resize_reflow_less_cols_wide_char_bulk_run) {
    ListHolder s(opts(8, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        /* A full row of wide character pairs so the reflow takes
         * the bulk run path. */
        widePairs(s->pages.first->page(), 4);
    }

    /* Resize to exactly two pairs per row: runs end on the row
     * boundary with no spacer heads needed. */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(pairsAt(page, 4));

        {
            const page::Row *row = page->getRowAndCell(0, 0).row;
            ASSERT_TRUE(row->wrap());
            ASSERT_FALSE(row->wrap_continuation());
        }
        {
            const page::Row *row = page->getRowAndCell(0, 1).row;
            ASSERT_FALSE(row->wrap());
            ASSERT_TRUE(row->wrap_continuation());
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_wide_char_bulk_run_odd_cols_spacer_head) {
    ListHolder s(opts(8, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        widePairs(s->pages.first->page(), 4);
    }

    /* Resize to an odd number of columns: the bulk run must stop a
     * pair short of the row boundary and the slow path inserts a
     * spacer head in the final column. */
    ASSERT_TRUE(s->resize(rz(5, -1, true)));
    ASSERT_TRUE(5 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(pairsAt(page, 4));

        {
            ASSERT_TRUE(cellIs(page, 4, 0, 0, Wide::spacer_head));
            ASSERT_TRUE(page->getRowAndCell(4, 0).row->wrap());
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(4, 1);
            ASSERT_TRUE(Wide::narrow == rac.cell->wide());
            ASSERT_TRUE(rac.row->wrap_continuation());
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_wide_char_bulk_run_mixed_narrow_round_trip) {
    /* The emoji-in-prose shape: single wide pairs separated by
     * narrow cells, which must all share a single bulk run. */
    struct Shape {
        uint32_t cp;
        Wide wide;
    };
    const Shape shape[] = {
        {0x4E00, Wide::wide}, {0, Wide::spacer_tail}, {'x', Wide::narrow},
        {0x4E01, Wide::wide}, {0, Wide::spacer_tail}, {'y', Wide::narrow},
        {0x4E02, Wide::wide}, {0, Wide::spacer_tail}, {'z', Wide::narrow},
    };

    ListHolder s(opts(9, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        for (size_t x = 0; x < 9; x++) *page->getRowAndCell(x, 0).cell = wideCell(shape[x].cp, shape[x].wide);
    }

    /* Shrink: the run ends exactly on the row boundary after the
     * second narrow cell. */
    ASSERT_TRUE(s->resize(rz(6, -1, true)));
    ASSERT_TRUE(6 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        for (size_t x = 0; x < 6; x++) ASSERT_TRUE(cellIs(page, x, 0, shape[x].cp, shape[x].wide));
        for (size_t x = 0; x < 3; x++) ASSERT_TRUE(cellIs(page, x, 1, shape[6 + x].cp, shape[6 + x].wide));
        ASSERT_TRUE(page->getRowAndCell(0, 0).row->wrap());
        ASSERT_TRUE(page->getRowAndCell(0, 1).row->wrap_continuation());
    }

    /* Grow back: the wrapped rows must rejoin into the original
     * single-row layout. */
    ASSERT_TRUE(s->resize(rz(9, -1, true)));
    ASSERT_TRUE(9 == s->cols);
    ASSERT_TRUE(1 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();
        for (size_t x = 0; x < 9; x++) ASSERT_TRUE(cellIs(page, x, 0, shape[x].cp, shape[x].wide));
        ASSERT_FALSE(page->getRowAndCell(0, 0).row->wrap());
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_wide_char_bulk_run_styled) {
    ListHolder s(opts(8, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* Create a style */
        style::Style st;
        st.flags.bold = true;
        style::Id style_id;
        ASSERT_TRUE(page->styles.add((const void *)page->memory, st, &style_id) == ref_counted_set::AddError::none);

        /* Styled pairs: the tail shares the wide cell's style, as
         * the print path writes them. */
        for (size_t i = 0; i < 4; i++) {
            page::Cell w = wideCell((uint32_t)(0x4E00 + i), Wide::wide);
            w.setStyleId(style_id);
            *page->getRowAndCell(i * 2, 0).cell = w;
            page->styles.use((const void *)page->memory, style_id);
            page::Cell t = wideCell(0, Wide::spacer_tail);
            t.setStyleId(style_id);
            *page->getRowAndCell(i * 2 + 1, 0).cell = t;
            page->styles.use((const void *)page->memory, style_id);
        }

        /* We're over-counted by 1 because `add` implies `use`. */
        page->styles.release((const void *)page->memory, style_id);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        for (size_t y = 0; y < 2; y++) {
            for (size_t x = 0; x < 4; x++) {
                const Page::RowAndCell rac = page->getRowAndCell(x, y);
                const style::Id style_id = rac.cell->style_id();
                ASSERT_TRUE(style_id != 0);

                ASSERT_TRUE(page->styles.get((const void *)page->memory, style_id)->flags.bold);
                ASSERT_TRUE(rac.row->styled());
                ASSERT_TRUE((x % 2 == 0 ? Wide::wide : Wide::spacer_tail) == rac.cell->wide());
            }
        }
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_wide_char_bulk_run_degenerate_spacer_tail_style) {
    ListHolder s(opts(4, 1, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* A styled wide cell whose tail does NOT share its style.
         * This can't come from the print path, but the run scan must
         * reject the pair (slow path) rather than rewrite the tail
         * to the wide cell's style. */
        style::Style st;
        st.flags.bold = true;
        style::Id style_id;
        ASSERT_TRUE(page->styles.add((const void *)page->memory, st, &style_id) == ref_counted_set::AddError::none);

        page::Cell w = wideCell(0x4E00, Wide::wide);
        w.setStyleId(style_id);
        *page->getRowAndCell(0, 0).cell = w;
        *page->getRowAndCell(1, 0).cell = wideCell(0, Wide::spacer_tail);
        *page->getRowAndCell(2, 0).cell = page::Cell::init('x');
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(3, -1, true)));
    ASSERT_TRUE(3 == s->cols);
    ASSERT_TRUE(1 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            ASSERT_TRUE(0x4E00 == rac.cell->contentCodepoint());
            ASSERT_TRUE(Wide::wide == rac.cell->wide());
            ASSERT_TRUE(rac.cell->style_id() != 0);
            ASSERT_TRUE(page->styles.get((const void *)page->memory, rac.cell->style_id())->flags.bold);
        }
        {
            const Page::RowAndCell rac = page->getRowAndCell(1, 0);
            ASSERT_TRUE(Wide::spacer_tail == rac.cell->wide());
            ASSERT_TRUE(style::default_id == rac.cell->style_id());
        }
        ASSERT_TRUE(cellIs(page, 2, 0, 'x', Wide::narrow));
    }
}

static bool familyGrapheme(Page *page, size_t x, size_t y) {
    const Page::RowAndCell rac = page->getRowAndCell(x, y);
    if (rac.cell->contentCodepoint() != 0x1F468 || rac.cell->wide() != Wide::wide) return false;
    size_t len = 0;
    const uint32_t *cps = page->lookupGrapheme(rac.cell, &len);
    const uint32_t expected[6] = {0x200D, 0x1F468, 0x200D, 0x1F466, 0x200D, 0x1F466};
    if (!cps || len != 6) return false;
    for (size_t i = 0; i < 6; i++)
        if (cps[i] != expected[i]) return false;
    return true;
}

TEST(page_list, PageList_resize_reflow_less_cols_to_wrap_a_multi_codepoint_grapheme_with_a_spacer_head) {
    ListHolder s(opts(4, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        /* We want to make the screen look like this:
         *
         * 👨‍👨‍👦‍👦👨‍👨‍👦‍👦 */
        const uint32_t rest[6] = {0x200D, 0x1F468, 0x200D, 0x1F466, 0x200D, 0x1F466};

        /* First family emoji at (0, 0) */
        {
            const Page::RowAndCell rac = page->getRowAndCell(0, 0);
            *rac.cell = wideCell(0x1F468, Wide::wide); /* First codepoint of the grapheme */
            ASSERT_TRUE(page->setGraphemes(rac.row, rac.cell, rest, 6) == page::PageError::none);
        }
        *page->getRowAndCell(1, 0).cell = wideCell(0, Wide::spacer_tail);
        /* Second family emoji at (2, 0) */
        {
            const Page::RowAndCell rac = page->getRowAndCell(2, 0);
            *rac.cell = wideCell(0x1F468, Wide::wide); /* First codepoint of the grapheme */
            ASSERT_TRUE(page->setGraphemes(rac.row, rac.cell, rest, 6) == page::PageError::none);
        }
        *page->getRowAndCell(3, 0).cell = wideCell(0, Wide::spacer_tail);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(3, -1, true)));
    ASSERT_TRUE(3 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        Page *page = s->pages.first->page();

        ASSERT_TRUE(familyGrapheme(page, 0, 0));
        /* Row should be wrapped */
        ASSERT_TRUE(page->getRowAndCell(0, 0).row->wrap());
        ASSERT_TRUE(cellIs(page, 1, 0, 0, Wide::spacer_tail));
        ASSERT_TRUE(cellIs(page, 2, 0, 0, Wide::spacer_head));

        ASSERT_TRUE(familyGrapheme(page, 0, 0));
        ASSERT_TRUE(cellIs(page, 1, 1, 0, Wide::spacer_tail));
    }
}

static void placeholders(PageList &s, Page *page, size_t x0, size_t x1) {
    (void)s;
    for (size_t x = x0; x < x1; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        rac.row->setKittyVirtualPlaceholder(true);
        *rac.cell = page::Cell::init(kitty_placeholder);
    }
}

TEST(page_list, PageList_resize_reflow_less_cols_copy_kitty_placeholder) {
    ListHolder s(opts(4, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        /* Write unicode placeholders */
        placeholders(*s, s->pages.first->page(), 0, (size_t)s->cols - 1);
    }

    /* Resize */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    Pin offset;
    while (it.next(&offset)) {
        for (size_t x = 0; x < (size_t)s->cols - 1; x++) {
            Pin offset_copy = offset;
            offset_copy.x = (size::CellCountInt)x;
            ASSERT_TRUE(offset_copy.rowAndCell().row->kitty_virtual_placeholder());
        }
    }
}

TEST(page_list, PageList_resize_reflow_more_cols_clears_kitty_placeholder) {
    ListHolder s(opts(4, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        /* Write unicode placeholders */
        placeholders(*s, s->pages.first->page(), 0, (size_t)s->cols - 1);
    }

    /* Resize smaller then larger */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    Pin row;
    ASSERT_TRUE(it.next(&row));
    ASSERT_TRUE(row.rowAndCell().row->kitty_virtual_placeholder());
    ASSERT_TRUE(it.next(&row));
    ASSERT_FALSE(row.rowAndCell().row->kitty_virtual_placeholder());
    ASSERT_FALSE(it.next(&row));
}

TEST(page_list, PageList_resize_reflow_wrap_moves_kitty_placeholder) {
    ListHolder s(opts(4, 2, (size_t)0));
    {
        ASSERT_TRUE(s->pages.first == s->pages.last);
        /* Write unicode placeholders */
        placeholders(*s, s->pages.first->page(), 2, (size_t)s->cols - 1);
    }

    ASSERT_TRUE(s->resize(rz(2, -1, true)));
    ASSERT_TRUE(2 == s->cols);
    ASSERT_TRUE(2 == s->totalRows());

    PageList::RowIterator it = s->rowIterator(Dir::right_down, Point::active());
    Pin row;
    ASSERT_TRUE(it.next(&row));
    ASSERT_FALSE(row.rowAndCell().row->kitty_virtual_placeholder());
    ASSERT_TRUE(it.next(&row));
    ASSERT_TRUE(row.rowAndCell().row->kitty_virtual_placeholder());
    ASSERT_FALSE(it.next(&row));
}

TEST(page_list, PageList_reset) {
    ListHolder s(opts(80, 24));
    s->reset();
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Active area should be the top */
    ASSERT_TRUE(pinEq(Pin(s->pages.first, 0, 0), s->getTopLeft(point::Tag::active)));
}

TEST(page_list, PageList_reset_invalidates_stale_untracked_refs_even_if_node_memory_is_reused) {
    ListHolder s(opts(80, 24));

    const size_t cap = PageList::page_preheat * 4;
    Node *stale_nodes[PageList::page_preheat * 4];
    uint64_t stale_serials[PageList::page_preheat * 4];
    size_t stale_len = 0;
    bool reused = false;
    Node *old_node = nullptr;
    uint64_t old_serial = 0;

    while (stale_len < cap && !reused) {
        Node *n = s->pages.first;
        const uint64_t ser = n->serial;
        ASSERT_TRUE(ser >= s->page_serial_epoch);
        ASSERT_TRUE(ser < s->page_serial);
        stale_nodes[stale_len] = n;
        stale_serials[stale_len] = ser;
        stale_len += 1;

        s->reset();

        Node *new_node = s->pages.first;
        for (size_t i = 0; i < stale_len; i++) {
            if (stale_nodes[i] == new_node) {
                reused = true;
                old_node = stale_nodes[i];
                old_serial = stale_serials[i];
                break;
            }
        }
    }

    ASSERT_TRUE(reused);
    Node *new_node = s->pages.first;
    const uint64_t new_serial = new_node->serial;

    /* Reset advances the epoch before rebuilding from the node pool. Reject
     * the stale generation before inspecting its pointer, even when that exact
     * address now belongs to a new live generation. */
    ASSERT_TRUE(old_node == new_node);
    ASSERT_TRUE(old_serial < s->page_serial_epoch);
    ASSERT_FALSE(s->nodeIsValid(old_node, old_serial));
    ASSERT_TRUE(s->nodeIsValid(new_node, new_serial));
    ASSERT_TRUE(new_serial >= s->page_serial_epoch);
    ASSERT_TRUE(new_serial < s->page_serial);
}

TEST(page_list, PageList_reset_across_two_pages) {
    /* Find a cap that makes it so that rows don't fit on one page. */
    const size::CellCountInt rows = 100;
    Capacity cap = stdAdjust(50);
    while (cap.rows >= rows) cap = stdAdjust((size::CellCountInt)(cap.cols + 50));

    /* Init */
    ListHolder s(opts(cap.cols, rows));
    s->reset();
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());
}

TEST(page_list, PageList_reset_moves_tracked_pins_and_marks_them_as_garbage) {
    ListHolder s(opts(80, 24));

    /* Create a tracked pin into the active area */
    Pin *p = s->trackPin(s->pin(Point::active(42, 12)).value);

    s->reset();

    /* Our added pin should now be garbage */
    ASSERT_TRUE(p->garbage);

    /* Viewport pin should not be garbage because it makes sense. */
    ASSERT_FALSE(s->viewport_pin->garbage);
    s->untrackPin(p);
}

TEST(page_list, PageList_clears_history) {
    ListHolder s(opts(80, 24));
    ASSERT_TRUE(s->growRows(30));
    s->reset();
    ASSERT_TRUE(s->viewport == Viewport::active);
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());

    /* Active area should be the top */
    ASSERT_TRUE(pinEq(Pin(s->pages.first, 0, 0), s->getTopLeft(point::Tag::active)));
}

TEST(page_list, PageList_resize_reflow_grapheme_map_capacity_exceeded) {
    /* This test verifies that when reflowing content with many graphemes,
     * the grapheme map capacity is correctly increased when needed. */
    ListHolder s(opts(4, 10, (size_t)0));
    ASSERT_TRUE(1 == s->totalPages());

    /* Get the grapheme capacity from the page. We need more than this many
     * graphemes in a single destination page to trigger capacity increase
     * during reflow. Since each source page can only hold this many graphemes,
     * we create two source pages with graphemes that will merge into one
     * destination page. */
    const size_t grapheme_capacity = s->pages.first->page()->graphemeCapacity();
    /* Use slightly more than half the capacity per page, so combined they
     * exceed the capacity of a single destination page. */
    const size_t graphemes_per_page = grapheme_capacity / 2 + grapheme_capacity / 4;

    /* Grow to the capacity of the first page and add more rows
     * so that we have two pages total. */
    ASSERT_TRUE(growFirstPageThen(*s, graphemes_per_page));
    ASSERT_TRUE(s->pages.first != s->pages.last);
    ASSERT_TRUE(s->pages.last == s->pages.first->next);

    /* Add graphemes to both pages. We add graphemes to rows at the END of the
     * first page, and graphemes to rows at the START of the second page.
     * When reflowing to 2 columns, these rows will wrap and stay together
     * on the same destination page, requiring capacity increase. */

    /* Add graphemes to the end of the first page (last rows) */
    {
        Page *page = s->pages.first->page();
        const size_t start_row = page->size.rows - graphemes_per_page;
        for (size_t i = 0; i < graphemes_per_page; i++) {
            const Page::RowAndCell rac = page->getRowAndCell(0, start_row + i);
            *rac.cell = page::Cell::init('A');
            ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0301) == page::PageError::none);
        }
    }

    /* Add graphemes to the beginning of the second page */
    {
        Page *page = s->pages.last->page();
        const size_t count = graphemes_per_page < page->size.rows ? graphemes_per_page : page->size.rows;
        for (size_t y = 0; y < count; y++) {
            const Page::RowAndCell rac = page->getRowAndCell(0, y);
            *rac.cell = page::Cell::init('B');
            ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0302) == page::PageError::none);
        }
    }

    /* Resize to fewer columns to trigger reflow.
     * The graphemes from both pages will be copied to destination pages.
     * They will all end up in a contiguous region of the destination.
     * If the bug exists (hyperlink_bytes increased instead of grapheme_bytes),
     * this will fail with GraphemeMapOutOfMemory when we exceed capacity. */
    ASSERT_TRUE(s->resize(rz(2, -1, true)));

    /* Verify the resize succeeded */
    ASSERT_TRUE(2 == s->cols);
}

static Node *compactNode(PageList &s, Node *node) {
    Node *out = nullptr;
    const bool ok = s.compact(node, &out);
    assert(ok);
    (void)ok;
    return out;
}

static void writeRowY(Page *page) {
    for (size_t y = 0; y < page->size.rows; y++) *page->getRowAndCell(0, y).cell = page::Cell::init((uint32_t)y);
}

TEST(page_list, PageList_resize_grow_cols_with_unwrap_fixes_viewport_pin) {
    /* Regression test: after resize/reflow, the viewport pin can end up at a
     * position where pin.y + rows > total_rows, causing getBottomRight to panic.
     *
     * The plan is to pin viewport in history, then grow columns to unwrap rows.
     * The unwrap reduces total_rows, but the tracked pin moves to a position
     * that no longer has enough rows below it for the viewport height. */
    ListHolder s(opts(2, 10));

    /* Make sure we have some history, in this case we have 30 rows of history */
    ASSERT_TRUE(s->growRows(30));
    ASSERT_TRUE(40 == s->totalRows());

    /* Fill all rows with wrapped content (pairs that unwrap when cols increase) */
    PageList::PageIterator it = s->pageIterator(Dir::right_down, Point::screen());
    PageList::Chunk chunk;
    while (it.next(&chunk)) {
        Page *page = chunk.node->page();
        for (size_t y = chunk.start; y < chunk.end; y++) {
            const Page::RowAndCell rac = page->getRowAndCell(0, y);
            if (y % 2 == 0) {
                rac.row->setWrap(true);
            } else {
                rac.row->setWrapContinuation(true);
            }
            for (size_t x = 0; x < s->cols; x++) *page->getRowAndCell(x, y).cell = page::Cell::init('A');
        }
    }

    /* Pin viewport at row 28 (in history, 2 rows before active area at row 30).
     * After unwrap: row 28 -> row 14, total_rows 40 -> 20, active starts at 10.
     * Pin at 14 needs rows 14-23, but only 0-19 exist -> overflow. */
    s->scroll(Scroll::pinAt(s->pin(Point::screen(0, 28)).value));
    ASSERT_TRUE(s->viewport == Viewport::pin);
    ASSERT_TRUE(s->getBottomRight(point::Tag::viewport).has);

    /* Resize with reflow: unwraps rows, reducing total_rows */
    ASSERT_TRUE(s->resize(rz(4, -1, true)));
    ASSERT_TRUE(4 == s->cols);
    ASSERT_TRUE(s->totalRows() < 40);

    /* Used to panic here, so test that we can get the bottom right. */
    ASSERT_TRUE(s->getBottomRight(point::Tag::viewport).has);
}

TEST(page_list, PageList_grow_reuses_non_standard_page_without_leak) {
    /* Create a PageList with 3 * std_size max so we can fit multiple pages
     * but will still trigger reuse. */
    ListHolder s(opts(80, 24, 3 * PageList::std_size));

    /* Increase the first page capacity to make it non-standard (larger than std_size). */
    while (s->pages.first->page()->memory_len <= PageList::std_size) {
        (void)incCap(*s, s->pages.first, IncCap::grapheme_bytes);
    }

    /* The first page should now have non-standard memory size. */
    ASSERT_TRUE(s->pages.first->page()->memory_len > PageList::std_size);

    /* First, fill up the first page's capacity */
    Node *first_page = s->pages.first;
    while (first_page->rows() < first_page->capacity().rows) (void)growNode(&*s);

    /* Now grow to create a second page */
    (void)growNode(&*s);
    ASSERT_TRUE(s->pages.first != s->pages.last);

    /* Continue growing until we exceed max_size AND the last page is full */
    while (s->page_size + kItem <= s->limits.max(Limits::Key::bytes) ||
           s->pages.last->rows() < s->pages.last->capacity().rows) {
        (void)growNode(&*s);
    }

    /* The first page should still be non-standard */
    ASSERT_TRUE(s->pages.first->page()->memory_len > PageList::std_size);

    /* Verify we have enough rows for active area (so prune path isn't skipped) */
    ASSERT_TRUE(s->totalRows() >= s->rows);

    /* Verify last page is full (so grow will need to allocate/reuse) */
    ASSERT_TRUE(s->pages.last->page()->size.rows == s->pages.last->capacity().rows);

    /* Remember the first page memory pointer before the reuse attempt */
    Node *first_page_ptr = s->pages.first;
    uint8_t *first_page_mem_ptr = s->pages.first->page()->memory;

    /* Create a tracked pin pointing to the non-standard first page */
    Pin *tracked_pin = s->trackPin(Pin(first_page_ptr, 0, 0));

    /* Now grow one more time to trigger the reuse path. Since the first page
     * is non-standard, it should be destroyed (not reused). The testing
     * allocator will detect a leak if destroyNode doesn't properly free
     * the non-standard memory. */
    (void)growNode(&*s);

    /* After grow, check if the first page is a different one
     * (meaning the non-standard page was pruned, not reused at the end)
     * The original first page should no longer be the first page */
    ASSERT_TRUE(s->pages.first != first_page_ptr);

    /* If the non-standard page was properly destroyed and not reused,
     * the last page should not have the same memory pointer */
    ASSERT_TRUE(s->pages.last->page()->memory != first_page_mem_ptr);

    /* The tracked pin should have been moved to the new first page and marked as garbage */
    ASSERT_TRUE(s->pages.first == tracked_pin->node);
    ASSERT_TRUE(0 == tracked_pin->x);
    ASSERT_TRUE(0 == tracked_pin->y);
    ASSERT_TRUE(tracked_pin->garbage);
    s->untrackPin(tracked_pin);
}

TEST(page_list, PageList_grow_non_standard_page_prune_protection) {
    /* This test specifically verifies the fix for the bug where pruning a
     * non-standard page would cause totalRows() < self.rows.
     *
     * Bug trigger conditions (all must be true simultaneously):
     * 1. first page is non-standard (memory.len > std_size)
     * 2. page_size + PagePool.item_size > maxSize() (triggers prune consideration)
     * 3. pages.first != pages.last (have multiple pages)
     * 4. total_rows >= self.rows (have enough rows for active area)
     * 5. total_rows - first.size.rows + 1 < self.rows (prune would lose too many) */

    /* This is kind of magic and likely depends on std_size. */
    const size::CellCountInt rows_count = 600;
    ListHolder s(opts(80, rows_count, PageList::std_size));

    /* Make the first page non-standard */
    while (s->pages.first->page()->memory_len <= PageList::std_size) {
        (void)incCap(*s, s->pages.first, IncCap::grapheme_bytes);
    }
    ASSERT_TRUE(s->pages.first->page()->memory_len > PageList::std_size);

    Node *first_page_node = s->pages.first;
    const size_t first_page_cap = first_page_node->capacity().rows;

    /* Fill first page to capacity */
    while (first_page_node->rows() < first_page_cap) (void)growNode(&*s);

    /* Grow until we have a second page (first page fills up first) */
    while (s->pages.first == s->pages.last) (void)growNode(&*s);
    ASSERT_TRUE(s->pages.first != s->pages.last);

    /* Fill the second page to capacity so that the next grow() triggers prune */
    Node *last_node = s->pages.last;
    const size_t second_cap = last_node->capacity().rows;
    while (last_node->rows() < second_cap) (void)growNode(&*s);

    /* Now the last page is full. The next grow must either:
     * 1. Prune the first page and reuse it, OR
     * 2. Allocate a new page */
    const size_t total = s->totalRows();
    const size_t would_remain = total - first_page_cap + 1;

    /* Verify the bug condition is present: pruning first page would leave < rows */
    ASSERT_TRUE(would_remain < s->rows);

    /* Verify prune path conditions are met */
    ASSERT_TRUE(s->pages.first != s->pages.last);
    ASSERT_TRUE(s->page_size + kItem > s->limits.max(Limits::Key::bytes));
    ASSERT_TRUE(s->totalRows() >= s->rows);

    /* Verify last page is at capacity (so grow must prune or allocate new) */
    ASSERT_TRUE(second_cap == last_node->rows());

    /* The next grow should trigger prune consideration.
     * Without the fix, this would destroy the non-standard first page,
     * leaving only second_cap + 1 rows, which is < self.rows. */
    (void)growNode(&*s);

    /* Verify the invariant holds - the fix prevents the destructive prune */
    ASSERT_TRUE(s->totalRows() >= s->rows);
}

TEST(page_list, PageList_resize_no_reflow_more_cols_remaps_pins_in_backfill_path) {
    /* Regression test: when resizeWithoutReflowGrowCols copies rows to a previous
     * page with spare capacity, tracked pins in those rows must be remapped.
     * Without the fix, pins become dangling pointers when the original page is destroyed. */
    const size::CellCountInt cols = 5;
    const Capacity cap = stdAdjust(cols);
    ListHolder s(opts(cols, cap.rows));

    /* Grow until we have two pages. */
    while (s->pages.first == s->pages.last) (void)growNode(&*s);
    Node *first_page = s->pages.first;
    Node *second_page = s->pages.last;
    ASSERT_TRUE(first_page != second_page);

    /* Trim a history row so the first page has spare capacity.
     * This triggers the backfill path in resizeWithoutReflowGrowCols. */
    s->eraseHistory(Point::history(0, 0));
    ASSERT_TRUE(first_page->rows() < first_page->capacity().rows);

    /* Ensure the resize takes the slow path (new capacity > current capacity). */
    const size::CellCountInt new_cols = cols + 1;
    Capacity adjusted;
    ASSERT_TRUE(second_page->capacity().adjust(Capacity::Adjustment::withCols(new_cols), &adjusted));
    ASSERT_TRUE(second_page->capacity().cols < adjusted.cols);

    /* Track a pin in row 0 of the second page. This row will be copied
     * to the first page during backfill and the pin must be remapped. */
    Pin *tracked = s->trackPin(Pin(second_page, 0, 0));

    /* Write a marker character to the tracked cell so we can verify
     * the pin points to the correct cell after resize. */
    const uint32_t marker = 'X';
    *tracked->rowAndCell().cell = page::Cell::init(marker);

    ASSERT_TRUE(s->resize(rz(new_cols, -1, false)));

    /* Verify the pin points to a valid node still in the page list. */
    bool found = false;
    for (Node *node = s->pages.first; node; node = node->next) {
        if (node == tracked->node) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(tracked->y < tracked->node->rows());

    /* Verify the pin still points to the cell with our marker content. */
    const page::Cell *cell = tracked->rowAndCell().cell;
    ASSERT_TRUE(page::Cell::ContentTag::codepoint == cell->content_tag());
    ASSERT_TRUE(marker == cell->contentCodepoint());
    s->untrackPin(tracked);
}

TEST(page_list, PageList_compact_pool_page_produces_exact_size_heap_page) {
    ListHolder s(opts(80, 24, (size_t)0));

    /* A freshly created page is pool-owned at std_size. */
    Node *node = s->pages.first;
    ASSERT_TRUE(Node::Owned::pool == node->owned);
    ASSERT_TRUE(node->page()->memory_len <= PageList::std_size);
    const page::Size original_size = node->page()->size;

    /* Compacting it should produce a much smaller exact-size heap page. */
    Node *new_node = compactNode(*s, node);
    ASSERT_TRUE(new_node != nullptr);
    ASSERT_TRUE(Node::Owned::heap == new_node->owned);
    ASSERT_TRUE(new_node->page()->memory_len < PageList::std_size);
    ASSERT_TRUE(original_size.rows == new_node->rows());
    ASSERT_TRUE(original_size.cols == new_node->cols());
    ASSERT_TRUE(new_node == s->pages.first);

    /* Our page size accounting should exactly match the compacted
     * page since it is the only page in the list. */
    ASSERT_TRUE(new_node->page()->memory_len == s->page_size);

    /* Compacting again should be a no-op since it is already exact. */
    ASSERT_TRUE(nullptr == compactNode(*s, new_node));
}

TEST(page_list, PageList_compact_then_grow_allocates_new_page) {
    ListHolder s(opts(80, 24));

    /* Compact the only page. It now has no spare row capacity. */
    Node *node = compactNode(*s, s->pages.first);
    ASSERT_TRUE(node->rows() == node->capacity().rows);

    /* Growing must allocate a fresh standard page from the pool,
     * exercising that a compacted page remains a valid live page. */
    (void)growNode(&*s);
    ASSERT_TRUE(s->pages.first != s->pages.last);
    ASSERT_TRUE(Node::Owned::pool == s->pages.last->owned);
    ASSERT_TRUE(25 == s->totalRows());
}

TEST(page_list, PageList_compact_then_reset_frees_heap_pages) {
    ListHolder s(opts(80, 24, (size_t)0));

    /* Compact the only page so the list contains a sub-std_size
     * heap-owned page. */
    Node *node = compactNode(*s, s->pages.first);
    ASSERT_TRUE(Node::Owned::heap == node->owned);
    ASSERT_TRUE(node->page()->memory_len < PageList::std_size);

    /* Reset must free the heap page (testing allocator catches leaks
     * and invalid frees) and rebuild from the pool. */
    s->reset();
    ASSERT_TRUE(Node::Owned::pool == s->pages.first->owned);
    ASSERT_TRUE((size_t)s->rows == s->totalRows());
}

TEST(page_list, PageList_compact_then_clone) {
    ListHolder s(opts(80, 24));

    /* Write a marker so we can verify contents survive. */
    *s->pages.first->page()->getRowAndCell(1, 2).cell = page::Cell::init('X');

    /* Compact so the source list contains a sub-std_size heap page. */
    Node *node = compactNode(*s, s->pages.first);
    ASSERT_TRUE(Node::Owned::heap == node->owned);
    ASSERT_TRUE(node->page()->memory_len < PageList::std_size);

    PageList s2;
    ASSERT_TRUE(s->clone(talloc(), cloneOpts(Point::screen()), &s2) == page::PageError::none);
    ASSERT_TRUE((size_t)s->rows == s2.totalRows());

    /* Verify the marker survived the clone. */
    ASSERT_TRUE('X' == s2.pages.first->page()->getRowAndCell(1, 2).cell->contentCodepoint());
    s2.deinit();
}

TEST(page_list, PageList_compact_oversized_page) {
    ListHolder s(opts(80, 24));

    /* Grow until we have multiple pages */
    Node *page1_node = s->pages.first;
    page1_node->page()->pauseIntegrityChecks(true);
    {
        const size_t n = (size_t)page1_node->capacity().rows - page1_node->rows();
        for (size_t i = 0; i < n; i++) (void)growNode(&*s);
    }
    page1_node->page()->pauseIntegrityChecks(false);
    (void)growNode(&*s);
    ASSERT_TRUE(s->pages.first != s->pages.last);

    Node *node = s->pages.first;

    /* Write content to verify it's preserved */
    {
        Page *page = node->page();
        for (size_t y = 0; y < page->size.rows; y++) {
            for (size_t x = 0; x < s->cols; x++) {
                *page->getRowAndCell(x, y).cell = page::Cell::init((uint32_t)(x + y * s->cols));
            }
        }
    }

    /* Create a tracked pin on this page */
    Pin *tracked = s->trackPin(Pin(node, 10, 5));

    /* Make the page oversized */
    while (node->page()->memory_len <= PageList::std_size) {
        node = incCap(*s, node, IncCap::grapheme_bytes);
    }
    ASSERT_TRUE(node->page()->memory_len > PageList::std_size);
    const size_t oversized_len = node->page()->memory_len;
    const page::Size original_size = node->page()->size;
    Node *second_node = node->next;

    /* Set dirty flag after increaseCapacity */
    node->page()->dirty = true;

    /* Compact the page */
    Node *new_node = compactNode(*s, node);
    ASSERT_TRUE(new_node != nullptr);

    /* Verify memory is smaller */
    ASSERT_TRUE(new_node->page()->memory_len < oversized_len);

    /* Verify size preserved */
    ASSERT_TRUE(original_size.rows == new_node->rows());
    ASSERT_TRUE(original_size.cols == new_node->cols());

    /* Verify dirty flag preserved */
    ASSERT_TRUE(new_node->page()->dirty);

    /* Verify linked list integrity */
    ASSERT_TRUE(new_node == s->pages.first);
    ASSERT_TRUE(nullptr == new_node->prev);
    ASSERT_TRUE(second_node == new_node->next);
    ASSERT_TRUE(new_node == second_node->prev);

    /* Verify pin updated correctly */
    ASSERT_TRUE(new_node == tracked->node);
    ASSERT_TRUE(5 == tracked->x);
    ASSERT_TRUE(10 == tracked->y);

    /* Verify content preserved */
    Page *page = new_node->page();
    for (size_t y = 0; y < page->size.rows; y++) {
        for (size_t x = 0; x < s->cols; x++) {
            ASSERT_TRUE((uint32_t)(x + y * s->cols) == page->getRowAndCell(x, y).cell->contentCodepoint());
        }
    }
    s->untrackPin(tracked);
}

TEST(page_list, PageList_destroyed_pool_page_reuse_is_zeroed) {
    ListHolder s(opts(80, 24));

    /* Create a page and scribble over its entire backing memory,
     * then destroy it so the buffer returns to the pool free list. */
    Node *node = s->createPage(PageList::CreatePage(PageList::initialCapacity(80)));
    node->page()->size.rows = 1;
    uint8_t *mem_ptr = node->page()->memory;
    memset(node->page()->memory, 0xAA, node->page()->memory_len);
    s->destroyNode(node);

    /* Reusing the buffer must produce a fully valid, zeroed page. */
    Node *node2 = s->createPage(PageList::CreatePage(PageList::initialCapacity(80)));
    ASSERT_TRUE(mem_ptr == node2->page()->memory);
    node2->page()->size.rows = node2->capacity().rows;

    const size_t cells_len = (size_t)node2->capacity().cols * node2->capacity().rows;
    const page::Cell *cells = node2->page()->cells.ptr(node2->page()->memory);
    bool zero = true;
    for (size_t i = 0; i < cells_len; i++)
        if (cells[i].bits != 0) zero = false;
    ASSERT_TRUE(zero);
    node2->page()->assertIntegrity();
    s->destroyNode(node2);
}

TEST(page_list, PageList_increaseCapacity_from_zero_capacity_dimensions) {
    ListHolder s(opts(80, 24, (size_t)0));

    /* Compact the only page. A plain page has no styled, grapheme,
     * or hyperlink content so the exact capacity is zero in every
     * managed dimension. */
    Node *node = compactNode(*s, s->pages.first);
    ASSERT_TRUE(0 == node->capacity().styles);
    ASSERT_TRUE(0 == node->capacity().grapheme_bytes);
    ASSERT_TRUE(0 == node->capacity().string_bytes);
    ASSERT_TRUE(0 == node->capacity().hyperlink_bytes);

    /* Increasing each dimension from zero must actually grow it.
     * Regression: 0 * 2 == 0 used to "succeed" without growing,
     * which turned caller retry loops into infinite loops. */
    node = incCap(*s, node, IncCap::styles);
    ASSERT_TRUE(node->capacity().styles > 0);
    node = incCap(*s, node, IncCap::grapheme_bytes);
    ASSERT_TRUE(node->capacity().grapheme_bytes > 0);
    node = incCap(*s, node, IncCap::string_bytes);
    ASSERT_TRUE(node->capacity().string_bytes > 0);
    node = incCap(*s, node, IncCap::hyperlink_bytes);
    ASSERT_TRUE(node->capacity().hyperlink_bytes > 0);

    /* Increasing a non-zero dimension still doubles. */
    const size_t styles = node->capacity().styles;
    node = incCap(*s, node, IncCap::styles);
    ASSERT_TRUE(styles * 2 == node->capacity().styles);
}

TEST(page_list, PageList_compact_after_increaseCapacity) {
    ListHolder s(opts(80, 24, (size_t)0));

    Node *node = s->pages.first;

    /* Grow the page capacity. The content is unchanged, so compaction
     * should always shrink it back down to an exact-size heap page. */
    node = incCap(*s, node, IncCap::grapheme_bytes);
    const size_t grown_len = node->page()->memory_len;

    Node *new_node = compactNode(*s, node);
    ASSERT_TRUE(Node::Owned::heap == new_node->owned);
    ASSERT_TRUE(new_node->page()->memory_len < grown_len);
    ASSERT_TRUE(new_node->page()->memory_len < PageList::std_size);
}

TEST(page_list, PageList_split_at_middle_row) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Write content to rows: row 0 gets codepoint 0, row 1 gets 1, etc. */
    writeRowY(page);

    /* Split at row 5 (middle) */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    /* Verify two pages exist */
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE(s->pages.first->next != nullptr);

    Page *first_page = s->pages.first->page();
    Page *second_page = s->pages.first->next->page();

    /* First page should have rows 0-4 (5 rows) */
    ASSERT_TRUE(5 == first_page->size.rows);
    /* Second page should have rows 5-9 (5 rows) */
    ASSERT_TRUE(5 == second_page->size.rows);

    /* Verify content in first page is preserved (rows 0-4 have codepoints 0-4) */
    for (uint32_t y = 0; y < 5; y++) ASSERT_TRUE(y == first_page->getRowAndCell(0, y).cell->contentCodepoint());

    /* Verify content in second page (original rows 5-9, now at y=0-4) */
    for (uint32_t y = 0; y < 5; y++) ASSERT_TRUE(y + 5 == second_page->getRowAndCell(0, y).cell->contentCodepoint());
}

TEST(page_list, PageList_split_at_row_0_is_no_op) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Write content to all rows */
    writeRowY(page);

    /* Split at row 0 should be a no-op */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 0, 0)) == PageList::SplitError::none);

    /* Verify only one page exists (no split occurred) */
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE(s->pages.first->next == nullptr);

    /* Verify all content is still in the original page */
    ASSERT_TRUE(10 == page->size.rows);
    for (uint32_t y = 0; y < 10; y++) ASSERT_TRUE(y == page->getRowAndCell(0, y).cell->contentCodepoint());
}

TEST(page_list, PageList_split_at_last_row) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Write content to all rows */
    writeRowY(page);

    /* Split at last row (row 9) */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 9, 0)) == PageList::SplitError::none);

    /* Verify two pages exist */
    ASSERT_TRUE(s->pages.first != nullptr);
    ASSERT_TRUE(s->pages.first->next != nullptr);

    Page *first_page = s->pages.first->page();
    Page *second_page = s->pages.first->next->page();

    /* First page should have 9 rows */
    ASSERT_TRUE(9 == first_page->size.rows);
    /* Second page should have 1 row */
    ASSERT_TRUE(1 == second_page->size.rows);

    /* Verify content in second page (original row 9, now at y=0) */
    ASSERT_TRUE(9 == second_page->getRowAndCell(0, 0).cell->contentCodepoint());
}

TEST(page_list, PageList_split_single_row_page_returns_OutOfSpace) {
    /* Initialize with 1 row */
    ListHolder s(opts(10, 1, (size_t)0));

    ASSERT_TRUE(s->split(Pin(s->pages.first, 0, 0)) == PageList::SplitError::OutOfSpace);
}

TEST(page_list, PageList_split_moves_tracked_pins) {
    ListHolder s(opts(10, 10, (size_t)0));

    /* Track a pin at row 7 */
    Pin *tracked = s->trackPin(Pin(s->pages.first, 7, 3));

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    /* The tracked pin should now be in the second page */
    ASSERT_TRUE(tracked->node == s->pages.first->next);
    /* y should be adjusted: was 7, split at 5, so new y = 7 - 5 = 2 */
    ASSERT_TRUE(2 == tracked->y);
    /* x should remain unchanged */
    ASSERT_TRUE(3 == tracked->x);
    s->untrackPin(tracked);
}

static page::Cell bgPalette(uint8_t idx) {
    page::Cell c;
    c.setContentTag(page::Cell::ContentTag::bg_color_palette);
    c.setContentColorPalette(idx);
    return c;
}

static void markRowMeta(page::Row *row) {
    row->setWrap(true);
    row->setWrapContinuation(true);
    row->setSemanticPrompt(SP::prompt);
}

static bool rowDefaultMeta(const page::Row *row) {
    return !row->wrap() && !row->wrap_continuation() && row->semantic_prompt() == SP::none;
}

TEST(page_list, PageList_split_tracked_pin_before_split_point_unchanged) {
    ListHolder s(opts(10, 10, (size_t)0));

    Node *original_node = s->pages.first;

    /* Track a pin at row 2 (before the split point) */
    Pin *tracked = s->trackPin(Pin(original_node, 2, 5));

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(original_node, 5, 0)) == PageList::SplitError::none);

    /* The tracked pin should remain in the original page */
    ASSERT_TRUE(tracked->node == s->pages.first);
    /* y and x should be unchanged */
    ASSERT_TRUE(2 == tracked->y);
    ASSERT_TRUE(5 == tracked->x);
    s->untrackPin(tracked);
}

TEST(page_list, PageList_split_tracked_pin_at_split_point_moves_to_new_page) {
    ListHolder s(opts(10, 10, (size_t)0));

    Node *original_node = s->pages.first;

    /* Track a pin at the exact split point (row 5) */
    Pin *tracked = s->trackPin(Pin(original_node, 5, 4));

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(original_node, 5, 0)) == PageList::SplitError::none);

    /* The tracked pin should be in the new page */
    ASSERT_TRUE(tracked->node == s->pages.first->next);
    /* y should be 0 since it was at the split point: 5 - 5 = 0 */
    ASSERT_TRUE(0 == tracked->y);
    /* x should remain unchanged */
    ASSERT_TRUE(4 == tracked->x);
    s->untrackPin(tracked);
}

TEST(page_list, PageList_split_multiple_tracked_pins_across_regions) {
    ListHolder s(opts(10, 10, (size_t)0));

    Node *original_node = s->pages.first;

    /* Track multiple pins in different regions */
    Pin *pin_before = s->trackPin(Pin(original_node, 1, 0));
    Pin *pin_at_split = s->trackPin(Pin(original_node, 5, 2));
    Pin *pin_after1 = s->trackPin(Pin(original_node, 7, 3));
    Pin *pin_after2 = s->trackPin(Pin(original_node, 9, 8));

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(original_node, 5, 0)) == PageList::SplitError::none);

    Node *first_page = s->pages.first;
    Node *second_page = first_page->next;

    /* Pin before split point stays in original page */
    ASSERT_TRUE(pinAtNode(pin_before, first_page, 1, 0));

    /* Pin at split point moves to new page with y=0 */
    ASSERT_TRUE(pinAtNode(pin_at_split, second_page, 0, 2));

    /* Pins after split point move to new page with adjusted y */
    ASSERT_TRUE(pinAtNode(pin_after1, second_page, 2, 3)); /* 7 - 5 = 2 */
    ASSERT_TRUE(pinAtNode(pin_after2, second_page, 4, 8)); /* 9 - 5 = 4 */

    s->untrackPin(pin_after2);
    s->untrackPin(pin_after1);
    s->untrackPin(pin_at_split);
    s->untrackPin(pin_before);
}

TEST(page_list, PageList_split_tracked_viewport_pin_in_split_region_moves_correctly) {
    ListHolder s(opts(10, 10, (size_t)0));

    Node *original_node = s->pages.first;

    /* Set viewport_pin to row 7 (after split point) */
    s->viewport_pin->node = original_node;
    s->viewport_pin->y = 7;
    s->viewport_pin->x = 6;

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(original_node, 5, 0)) == PageList::SplitError::none);

    /* viewport_pin should be in the new page */
    ASSERT_TRUE(s->viewport_pin->node == s->pages.first->next);
    /* y should be adjusted: 7 - 5 = 2 */
    ASSERT_TRUE(2 == s->viewport_pin->y);
    /* x should remain unchanged */
    ASSERT_TRUE(6 == s->viewport_pin->x);
}

TEST(page_list, PageList_split_middle_page_preserves_linked_list_order) {
    /* Create a single page with 12 rows */
    ListHolder s(opts(10, 12, (size_t)0));

    /* Split at row 4 to create: page1 (rows 0-3), page2 (rows 4-11) */
    Node *first_node = s->pages.first;
    ASSERT_TRUE(s->split(Pin(first_node, 4, 0)) == PageList::SplitError::none);

    /* Now we have 2 pages */
    Node *page1 = s->pages.first;
    Node *page2 = s->pages.first->next;
    ASSERT_TRUE(4 == page1->rows());
    ASSERT_TRUE(8 == page2->rows());

    /* Split page2 at row 4 to create: page1 -> page2 (rows 0-3) -> page3 (rows 4-7) */
    ASSERT_TRUE(s->split(Pin(page2, 4, 0)) == PageList::SplitError::none);

    /* Now we have 3 pages */
    Node *first = s->pages.first;
    Node *middle = first->next;
    Node *last = middle->next;

    /* Verify linked list order: first -> middle -> last */
    ASSERT_TRUE(page1 == first);
    ASSERT_TRUE(page2 == middle);
    ASSERT_TRUE(s->pages.last == last);

    /* Verify prev pointers */
    ASSERT_TRUE(first->prev == nullptr);
    ASSERT_TRUE(first == middle->prev);
    ASSERT_TRUE(middle == last->prev);

    /* Verify next pointers */
    ASSERT_TRUE(middle == first->next);
    ASSERT_TRUE(last == middle->next);
    ASSERT_TRUE(last->next == nullptr);

    /* Verify row counts */
    ASSERT_TRUE(4 == first->rows());
    ASSERT_TRUE(4 == middle->rows());
    ASSERT_TRUE(4 == last->rows());
}

TEST(page_list, PageList_split_last_page_makes_new_page_the_last) {
    /* Create a single page with 10 rows */
    ListHolder s(opts(10, 10, (size_t)0));

    /* Split to create 2 pages first */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    /* Now split the last page */
    Node *last_before_split = s->pages.last;
    ASSERT_TRUE(5 == last_before_split->rows());

    ASSERT_TRUE(s->split(Pin(last_before_split, 2, 0)) == PageList::SplitError::none);

    /* The new page should be the new last */
    Node *new_last = s->pages.last;
    ASSERT_TRUE(new_last != last_before_split);
    ASSERT_TRUE(last_before_split == new_last->prev);
    ASSERT_TRUE(new_last->next == nullptr);

    /* Verify row counts: original last has 2 rows, new last has 3 rows */
    ASSERT_TRUE(2 == last_before_split->rows());
    ASSERT_TRUE(3 == new_last->rows());
}

TEST(page_list, PageList_split_first_page_keeps_original_as_first) {
    /* Create 2 pages by splitting */
    ListHolder s(opts(10, 10, (size_t)0));

    Node *original_first = s->pages.first;
    ASSERT_TRUE(s->split(Pin(original_first, 5, 0)) == PageList::SplitError::none);

    /* Get second page (created by first split) */
    Node *second_page = s->pages.first->next;

    /* Now split the first page again */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 2, 0)) == PageList::SplitError::none);

    /* Original first should still be first */
    ASSERT_TRUE(original_first == s->pages.first);
    ASSERT_TRUE(s->pages.first->prev == nullptr);

    /* New page should be inserted between first and second */
    Node *inserted = s->pages.first->next;
    ASSERT_TRUE(inserted != second_page);
    ASSERT_TRUE(second_page == inserted->next);

    /* Verify row counts: first has 2, inserted has 3, second has 5 */
    ASSERT_TRUE(2 == s->pages.first->rows());
    ASSERT_TRUE(3 == inserted->rows());
    ASSERT_TRUE(5 == second_page->rows());
}

TEST(page_list, PageList_split_preserves_wrap_flags) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Set wrap flags on rows that will be in the second page after split
     * Row 5: wrap = true (this is the start of a wrapped line)
     * Row 6: wrap_continuation = true (this continues the wrap)
     * Row 7: wrap = true, wrap_continuation = true (wrapped and continues) */
    page->getRowAndCell(0, 5).row->setWrap(true);
    page->getRowAndCell(0, 6).row->setWrapContinuation(true);
    page->getRowAndCell(0, 7).row->setWrap(true);
    page->getRowAndCell(0, 7).row->setWrapContinuation(true);

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    Page *second_page = s->pages.first->next->page();

    /* Verify wrap flags are preserved in new page
     * Original row 5 is now row 0 in second page */
    {
        const page::Row *r = second_page->getRowAndCell(0, 0).row;
        ASSERT_TRUE(r->wrap());
        ASSERT_FALSE(r->wrap_continuation());
    }

    /* Original row 6 is now row 1 in second page */
    {
        const page::Row *r = second_page->getRowAndCell(0, 1).row;
        ASSERT_FALSE(r->wrap());
        ASSERT_TRUE(r->wrap_continuation());
    }

    /* Original row 7 is now row 2 in second page */
    {
        const page::Row *r = second_page->getRowAndCell(0, 2).row;
        ASSERT_TRUE(r->wrap());
        ASSERT_TRUE(r->wrap_continuation());
    }
}

TEST(page_list, PageList_split_preserves_styled_cells) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Create a style and apply it to cells in rows 5-7 (which will be in the second page) */
    style::Style st;
    st.flags.bold = true;
    style::Id style_id;
    ASSERT_TRUE(page->styles.add((const void *)page->memory, st, &style_id) == ref_counted_set::AddError::none);

    for (size_t y = 5; y < 8; y++) {
        const Page::RowAndCell rac = page->getRowAndCell(0, y);
        page::Cell c = page::Cell::init('S');
        c.setStyleId(style_id);
        *rac.cell = c;
        rac.row->setStyled(true);
        page->styles.use((const void *)page->memory, style_id);
    }
    /* Release the extra ref from add */
    page->styles.release((const void *)page->memory, style_id);

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    Page *first_page = s->pages.first->page();
    Page *second_page = s->pages.first->next->page();

    /* First page should have no styles (all styled rows moved to second page) */
    ASSERT_TRUE(0 == first_page->styles.count());

    /* Second page should have exactly 1 style (the bold style, used by 3 cells) */
    ASSERT_TRUE(1 == second_page->styles.count());

    /* Verify styled cells are preserved in new page */
    for (size_t y = 0; y < 3; y++) {
        const Page::RowAndCell rac = second_page->getRowAndCell(0, y);
        ASSERT_TRUE('S' == rac.cell->contentCodepoint());
        ASSERT_TRUE(rac.cell->style_id() != 0);

        ASSERT_TRUE(second_page->styles.get((const void *)second_page->memory, rac.cell->style_id())->flags.bold);
        ASSERT_TRUE(rac.row->styled());
    }
}

TEST(page_list, PageList_split_preserves_grapheme_clusters) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Add a grapheme cluster to row 6 (will be row 1 in second page after split at 5) */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 6);
        *rac.cell = page::Cell::init(0x1F468); /* Man emoji */
        const uint32_t rest[2] = {
            0x200D,  /* ZWJ */
            0x1F469, /* Woman emoji */
        };
        ASSERT_TRUE(page->setGraphemes(rac.row, rac.cell, rest, 2) == page::PageError::none);
    }

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    Page *first_page = s->pages.first->page();
    Page *second_page = s->pages.first->next->page();

    /* First page should have no graphemes (the grapheme row moved to second page) */
    ASSERT_TRUE(0 == first_page->graphemeCount());

    /* Second page should have exactly 1 grapheme */
    ASSERT_TRUE(1 == second_page->graphemeCount());

    /* Verify grapheme is preserved in new page (original row 6 is now row 1) */
    {
        const Page::RowAndCell rac = second_page->getRowAndCell(0, 1);
        ASSERT_TRUE(0x1F468 == rac.cell->contentCodepoint());
        ASSERT_TRUE(rac.row->grapheme());

        size_t len = 0;
        const uint32_t *cps = second_page->lookupGrapheme(rac.cell, &len);
        ASSERT_TRUE(2 == len);
        ASSERT_TRUE(0x200D == cps[0]);
        ASSERT_TRUE(0x1F469 == cps[1]);
    }
}

TEST(page_list, PageList_split_preserves_hyperlinks) {
    ListHolder s(opts(10, 10, (size_t)0));

    Page *page = s->pages.first->page();

    /* Add a hyperlink to row 7 (will be row 2 in second page after split at 5) */
    hyperlink::Id hyperlink_id;
    ASSERT_TRUE(page->insertHyperlink(implicitLink("https://example.com", 0), &hyperlink_id) ==
                page::PageError::none);
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 7);
        *rac.cell = page::Cell::init('L');
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, hyperlink_id) == page::PageError::none);
    }

    /* Split at row 5 */
    ASSERT_TRUE(s->split(Pin(s->pages.first, 5, 0)) == PageList::SplitError::none);

    Page *first_page = s->pages.first->page();
    Page *second_page = s->pages.first->next->page();

    /* First page should have no hyperlinks (the hyperlink row moved to second page) */
    ASSERT_TRUE(0 == first_page->hyperlink_set.count());

    /* Second page should have exactly 1 hyperlink */
    ASSERT_TRUE(1 == second_page->hyperlink_set.count());

    /* Verify hyperlink is preserved in new page (original row 7 is now row 2) */
    {
        const Page::RowAndCell rac = second_page->getRowAndCell(0, 2);
        ASSERT_TRUE('L' == rac.cell->contentCodepoint());
        ASSERT_TRUE(rac.cell->hyperlink());

        hyperlink::Id link_id;
        ASSERT_TRUE(second_page->lookupHyperlink(rac.cell, &link_id));
        const hyperlink::PageEntry *link = second_page->hyperlink_set.get((const void *)second_page->memory, link_id);
        const char *expected = "https://example.com";
        ASSERT_TRUE(link->uri.len == strlen(expected));
        ASSERT_TRUE(memcmp(link->uri.slice((const void *)second_page->memory), expected, link->uri.len) == 0);
    }
}

TEST(page_list, PageList_eraseRow_recycled_row_has_default_metadata) {
    ListHolder s(opts(5, 3));

    /* Simulate the top row being part of a soft-wrapped, prompt-marked
     * line. Erasing it recycles its Row storage as the new blank
     * bottom row, which must not retain any of this metadata. */
    markRowMeta(s->getCell(Point::active()).value.row);

    s->eraseRow(Point::active());

    ASSERT_TRUE(rowDefaultMeta(s->getCell(Point::active(0, 2)).value.row));
}

TEST(page_list, PageList_eraseRowBounded_recycled_row_has_default_metadata) {
    /* A limit smaller than the remaining rows in the page exercises
     * the bounded-rotate branch; a larger limit exercises the fallback
     * branch that clears the final row after a full rotation. */
    const size_t limits_[] = {1, 10};
    for (size_t limit : limits_) {
        ListHolder s(opts(5, 3));

        markRowMeta(s->getCell(Point::active()).value.row);

        s->eraseRowBounded(Point::active(), limit);

        const size_t recycled_y = limit < 2 ? limit : 2;
        ASSERT_TRUE(rowDefaultMeta(s->getCell(Point::active(0, (uint32_t)recycled_y)).value.row));
    }
}

TEST(page_list, PageList_eraseActive_regrown_rows_have_default_metadata) {
    ListHolder s(opts(5, 3));

    /* Mark the rows that will be erased. eraseActive retires their
     * storage into unused page capacity and then regrows the active
     * area, re-exposing the same Row storage via the grow() fast path. */
    for (uint32_t y = 0; y < 2; y++) markRowMeta(s->getCell(Point::active(0, y)).value.row);

    s->eraseActive(1);

    for (uint32_t y = 0; y < 3; y++) ASSERT_TRUE(rowDefaultMeta(s->getCell(Point::active(0, y)).value.row));
}

TEST(page_list, PageList_split_retired_rows_have_default_state) {
    ListHolder s(opts(5, 10));

    /* Put metadata and a background-colored (non-zero, but text-free)
     * cell on a row that the split will move to the new page. The
     * retired Row storage on the source page goes back into unused
     * capacity that grow() re-exposes without clearing. */
    {
        const PageList::Cell rac = s->getCell(Point::active(0, 7)).value;
        rac.row->setWrap(true);
        rac.row->setSemanticPrompt(SP::prompt);
        *rac.cell = bgPalette(42);
    }

    Node *node = s->pages.first;
    ASSERT_TRUE(s->split(s->pin(Point::active(0, 5)).value) == PageList::SplitError::none);

    /* The source page was truncated to 5 rows; peek at the retired
     * storage beyond size.rows. */
    Page *page = node->page();
    ASSERT_TRUE(5 == page->size.rows);
    page::Row *rows = page->rows.ptr(page->memory);
    for (size_t y = 5; y < 10; y++) {
        const page::Row row = rows[y];
        ASSERT_TRUE(rowDefaultMeta(&row));
        const page::Cell *cells = row.cells().ptr(page->memory);
        for (size_t x = 0; x < page->size.cols; x++) ASSERT_TRUE(cells[x].isZero());
    }
}

TEST(page_list, PageList_resize_trimmed_rows_have_default_state) {
    ListHolder s(opts(5, 5));

    /* A trailing blank row has no text, so shrinking rows trims it,
     * but it can still carry metadata (e.g. a blank prompt
     * continuation line) and background-colored cells. Trimming
     * retires the storage into unused capacity that grow()
     * re-exposes without clearing. */
    {
        const PageList::Cell rac = s->getCell(Point::active(0, 4)).value;
        rac.row->setWrapContinuation(true);
        rac.row->setSemanticPrompt(SP::prompt_continuation);
        *rac.cell = bgPalette(42);
    }

    ASSERT_TRUE(s->resize(rz(-1, 4, false)));
    ASSERT_TRUE(s->resize(rz(-1, 5, false)));

    {
        const PageList::Cell rac = s->getCell(Point::active(0, 4)).value;
        ASSERT_TRUE(rowDefaultMeta(rac.row));
        ASSERT_TRUE(rac.cell->isZero());
    }
}

TEST(page_list, PageList_memory_pool_never_touches_idle_page_memory) {
    const size_t preheat = PageList::page_preheat;
    const size_t std_size = PageList::std_size;

    /* Back the page allocator with memory we can inspect. */
    uint8_t *backing = (uint8_t *)_aligned_malloc(preheat * std_size, 4096);
    zigstd::FixedBufferAllocator fba(backing, preheat * std_size);

    PageList::MemoryPool pool;
    ASSERT_TRUE(PageList::MemoryPool::init(talloc(), fba.allocator(), preheat, &pool));

    /* Preheat allocated exactly the items. */
    ASSERT_TRUE(preheat * std_size == fba.end_index);

    /* Lay the sentinel down after preheat: allocation itself may write
     * (the Allocator interface fills fresh memory with undefined in
     * safe builds, which valgrind also tracks). The sentinel must differ
     * from Zig's 0xAA undefined pattern so that any write is visible. */
    const uint8_t sentinel = 0x5A;
    memset(backing, sentinel, preheat * std_size);

    /* Every preheated item is handed out untouched and without going
     * back to the page allocator, and destroying it doesn't touch it. */
    PageList::PagePool::ItemPtr items[PageList::page_preheat];
    for (size_t i = 0; i < preheat; i++) {
        items[i] = pool.pages.create();
        ASSERT_TRUE(preheat * std_size == fba.end_index);
        bool same = true;
        for (size_t k = 0; k < std_size; k++)
            if (items[i]->bytes[k] != sentinel) same = false;
        ASSERT_TRUE(same);
    }
    for (size_t i = 0; i < preheat; i++) pool.pages.destroy(items[i]);
    bool untouched = true;
    for (size_t k = 0; k < preheat * std_size; k++)
        if (backing[k] != sentinel) untouched = false;
    ASSERT_TRUE(untouched);

    pool.deinit();
    _aligned_free(backing);
}

TEST(page_list, PageList_memory_pool_fast_path_does_not_allocate) {
    zigstd::FailingAllocator counting(talloc(), SIZE_MAX);

    PageList::MemoryPool pool;
    ASSERT_TRUE(PageList::MemoryPool::init(talloc(), counting.allocator(), PageList::page_preheat, &pool));
    ASSERT_TRUE(PageList::page_preheat == counting.allocations);

    /* Cycle a few thousand pages through the preheated items. As long
     * as no more than the preheat are live at once, create is a
     * free-list pop and never touches the page allocator. */
    PageList::PagePool::ItemPtr items[PageList::page_preheat];
    for (int k = 0; k < 1024; k++) {
        for (size_t i = 0; i < PageList::page_preheat; i++) items[i] = pool.pages.create();
        for (size_t i = 0; i < PageList::page_preheat; i++) pool.pages.destroy(items[i]);
    }
    ASSERT_TRUE(PageList::page_preheat == counting.allocations);
    ASSERT_TRUE(0 == counting.deallocations);

    /* Going past the preheat allocates the extra items once; they are
     * recycled from then on. */
    PageList::PagePool::ItemPtr extra[PageList::page_preheat + 2];
    const size_t extra_len = PageList::page_preheat + 2;
    for (int k = 0; k < 1024; k++) {
        for (size_t i = 0; i < extra_len; i++) extra[i] = pool.pages.create();
        for (size_t i = 0; i < extra_len; i++) pool.pages.destroy(extra[i]);
    }
    ASSERT_TRUE(extra_len == counting.allocations);
    ASSERT_TRUE(0 == counting.deallocations);
    pool.deinit();
}

/* Wisp: upstream's comptime invariant in initialCapacity and the
 * std_size constant this port hardcodes. */
TEST(page_list, Wisp_std_size_matches_layout_and_max_cols_fit) {
    ASSERT_TRUE(Page::layout(page::std_capacity()).total_size == PageList::std_size);
    Capacity cap = page::std_capacity();
    cap.cols = 0xFFFF;
    ASSERT_TRUE(Page::layout(cap).total_size <= size::max_page_size);
}

/* @@TESTS@@ */

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(page_list, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
