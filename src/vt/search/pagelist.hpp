/* Transliterated from Ghostty src/terminal/search/pagelist.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: departures from a mechanical mapping:
 *
 *   - `Allocator.Error!PageListSearch` is a bool return plus an
 *     out-parameter, as in sliding_window.hpp; false is OutOfMemory.
 *
 *   - `Allocator.Error!bool` (feed) is a bool return (false is
 *     OutOfMemory) plus a `bool *out` carrying upstream's value.
 *
 *   - `?FlattenedHighlight` (next) is a bool return with the value in an
 *     out-parameter; false is upstream's null.
 *
 *   - `errdefer` is an RAII guard.
 *
 *   - `rem -|= n` uses an explicit saturating subtraction; the port has no
 *     `-|=` operator.
 *
 *   - Upstream's `../main.zig` index import supplies `point`, `size`,
 *     `Page`, `PageList`, `Selection` and `Screen`, and `../Terminal.zig`
 *     supplies `Terminal`. Of those, only `PageList` (and `PageList::Pin`)
 *     is used outside the test blocks, so this header includes only
 *     sliding_window.hpp, which already brings in `PageList` by way of
 *     highlight.hpp. The test-only imports become includes in
 *     tests/test_vt_search_pagelist_port.cpp.
 */

#pragma once
#ifndef WISP_VT_SEARCH_PAGELIST_HPP
#define WISP_VT_SEARCH_PAGELIST_HPP

#include <stddef.h>
#include <stdint.h>

#include "sliding_window.hpp"

namespace wisp {
namespace vt {
namespace search {

/// Searches for a term in a PageList structure.
///
/// This searches in reverse order starting from the given node.
///
/// This assumes that nodes do not change contents. For nodes that change
/// contents, look at ActiveSearch, which is designed to re-search the active
/// area since it assumed to change. When integrating ActiveSearch with
/// PageListSearch, the caller should start the PageListSearch from the
/// returned node from ActiveSearch.update().
///
/// Concurrent access to a PageList or nodes in a PageList are not allowed,
/// so the caller should ensure that necessary locks are held. Each function
/// documents whether it accesses the PageList or not. For example, you can
/// safely call `next()` without holding a lock, but you must hold a lock
/// while calling `feed()`.
struct PageListSearch {
    typedef highlight::Flattened FlattenedHighlight;
    typedef PageList::Pin Pin;

    /// The list we're searching.
    PageList *list;

    /// The sliding window of page contents and nodes to search.
    SlidingWindow window;

    /// The tracked pin for our current position in the pagelist. This
    /// will always point to the CURRENT node we're searching from so that
    /// we can track if we move.
    Pin *pin;

    PageListSearch() : list(nullptr), window(), pin(nullptr) {}

    /// Initialize the page list search. The needle is copied so it can
    /// be freed immediately.
    ///
    /// Accesses the PageList/Node so the caller must ensure it is safe
    /// to do so if there is any concurrent access.
    ///
    /// Wisp: Allocator.Error!PageListSearch. false is OutOfMemory.
    static bool init(zigstd::Allocator alloc, const uint8_t *needle, size_t needle_len, PageList *list,
                     PageList::Node *start, PageListSearch *out) {
        // We put a tracked pin into the node that we're starting from.
        // By using a tracked pin, we can keep our pagelist references safe
        // because if the pagelist prunes pages, the tracked pin will
        // be moved somewhere safe.
        Pin *pin = list->trackPin(Pin(start, (size::CellCountInt)(start->rows() - 1),
                                      (size::CellCountInt)(start->cols() - 1)));
        if (!pin) return false;
        /* Wisp: errdefer list.untrackPin(pin); */
        struct PinGuard {
            PageList *list;
            Pin *pin;
            ~PinGuard() {
                if (pin) list->untrackPin(pin);
            }
        } pin_guard = {list, pin};

        // Create our sliding window we'll use for searching.
        SlidingWindow window;
        if (!SlidingWindow::init(alloc, SlidingWindow::Direction::reverse, needle, needle_len, &window)) {
            return false;
        }
        /* Wisp: errdefer window.deinit(); */
        struct WindowGuard {
            SlidingWindow *w;
            bool armed;
            ~WindowGuard() {
                if (armed) w->deinit();
            }
        } window_guard = {&window, true};

        // We always feed our initial page data into the window, because
        // we have the lock anyways and this lets our `pin` point to our
        // current node and feed to work properly.
        {
            SlidingWindow::AppendResult discard;
            if (!window.append(start, &discard)) return false;
        }

        window_guard.armed = false;
        pin_guard.pin = nullptr;

        out->list = list;
        out->window = window;
        out->pin = pin;
        return true;
    }

    /// Modifies the PageList (to untrack a pin) so the caller must ensure
    /// that it is safe to do so.
    void deinit() {
        window.deinit();
        list->untrackPin(pin);
    }

    /// Release owned search buffers after the PageList itself has already
    /// been destroyed. Its pin belonged to the PageList pool and was freed
    /// with that list, so it must not be untracked here.
    void deinitListInvalid() { window.deinit(); }

    /// Return the next match in the loaded page nodes. If this returns
    /// null then the PageList search needs to be fed the next node(s).
    /// Call, `feed` to do this.
    ///
    /// Beware that the selection returned may point to a node that
    /// is freed if the caller does not hold necessary locks on the
    /// PageList while searching. The pins should be validated prior to
    /// final use.
    ///
    /// This does NOT access the PageList, so it can be called without
    /// a lock held.
    ///
    /// Wisp: ?FlattenedHighlight. false is null.
    bool next(FlattenedHighlight *out) { return window.next(out); }

    /// Feed more data to the sliding window from the pagelist. This will
    /// feed enough data to cover at least one match (needle length) if it
    /// exists; this doesn't perform the search, it only feeds data.
    ///
    /// This accesses nodes in the PageList, so the caller must ensure
    /// it is safe to do so (i.e. hold necessary locks).
    ///
    /// This returns false if there is no more data to feed. This essentially
    /// means we've searched the entire pagelist.
    ///
    /// Wisp: Allocator.Error!bool. A false return is OutOfMemory; upstream's
    /// value is in `*out`.
    bool feed(bool *out) {
        *out = false;

        // If our pin becomes garbage it means wherever we were next
        // was reused and we can't make sense of our progress anymore.
        // It is effectively equivalent to reaching the end of the PageList.
        if (pin->garbage) return true;

        // Add at least enough data to find a single match.
        size_t rem = window.needle_len;

        // Start at our previous node and then continue adding until we
        // get our desired amount of data.
        for (PageList::Node *node = pin->node->prev; node != nullptr; node = node->prev) {
            SlidingWindow::AppendResult result;
            if (!window.append(node, &result)) return false;
            /* Wisp: rem -|= result.content_len */
            rem = rem > result.content_len ? rem - result.content_len : 0;

            // Move our tracked pin to the new node.
            pin->node = node;
            pin->y = (size::CellCountInt)(node->rows() - 1);
            pin->x = (size::CellCountInt)(node->cols() - 1);
            pin->garbage = false;

            if (rem == 0) break;
        }

        // True if we fed any data.
        *out = rem < window.needle_len;
        return true;
    }
};

} /* namespace search */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SEARCH_PAGELIST_HPP */
