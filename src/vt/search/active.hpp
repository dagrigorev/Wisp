/* Transliterated from Ghostty src/terminal/search/active.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: departures from a mechanical mapping:
 *
 *   - `Allocator.Error!T` is a bool return plus an out-parameter, as in
 *     sliding_window.hpp; false is OutOfMemory.
 *
 *   - `Allocator.Error!?*PageList.List.Node` is therefore a bool return
 *     plus a `PageList::Node **` out-parameter. Zig's `?*T` is a plain
 *     nullable pointer in C++, so no `Maybe<>` wrapper is needed: a true
 *     return with `*out == nullptr` is upstream's `null`.
 *
 *   - `PageList.List.Node` is spelled `PageList::Node` in this port, which
 *     is where the ported page_list.hpp put it.
 *
 *   - Zig's `while (opt) |x| : (opt = x.prev)` is a `while` whose body ends
 *     with the continue-expression. Upstream's first loop reassigns `node_`
 *     before its `break`, which the transliteration preserves verbatim
 *     because the loop's exit value is read afterwards.
 */

#pragma once
#ifndef WISP_VT_SEARCH_ACTIVE_HPP
#define WISP_VT_SEARCH_ACTIVE_HPP

#include <stddef.h>

#include "../page_list.hpp"
#include "sliding_window.hpp"

namespace wisp {
namespace vt {
namespace search {

/// Searches for a substring within the active area of a PageList.
///
/// The distinction for "active area" is important because it is the
/// only part of a PageList that is mutable. Therefore, its the only part
/// of the terminal that needs to be repeatedly searched as the contents
/// change.
///
/// This struct specializes in searching only within that active area,
/// and handling the active area moving as new lines are added to the bottom.
struct ActiveSearch {
    typedef highlight::Flattened FlattenedHighlight;

    SlidingWindow window;

    ActiveSearch() : window() {}

    /* Wisp: Allocator.Error!ActiveSearch. false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, const uint8_t *needle, size_t needle_len,
                     ActiveSearch *out) {
        // We just do a forward search since the active area is usually
        // pretty small so search results are instant anyways. This avoids
        // a small amount of work to reverse things.
        SlidingWindow window;
        if (!SlidingWindow::init(alloc, SlidingWindow::Direction::forward, needle, needle_len,
                                 &window))
            return false;
        /* Wisp: errdefer window.deinit() — nothing after this can fail. */
        out->window = window;
        return true;
    }

    void deinit() { window.deinit(); }

    /// Update the active area to reflect the current state of the PageList.
    ///
    /// This doesn't do the search, it only copies the necessary data
    /// to perform the search later. This lets the caller hold the lock
    /// on the PageList for a minimal amount of time.
    ///
    /// This returns the first page (in reverse order) covered by this
    /// search. This allows the history search to overlap and search history.
    /// There CAN BE duplicates, and this page CAN BE mutable, so the history
    /// search results should prune anything that's in the active area.
    ///
    /// If the return value is null it means the active area covers the entire
    /// PageList, currently.
    ///
    /// Wisp: Allocator.Error!?*PageList.List.Node. false is OutOfMemory; a
    /// true return with `*out == nullptr` is upstream's null.
    bool update(const PageList *list, PageList::Node **out) {
        // Clear our previous sliding window
        window.clearAndRetainCapacity();

        // An empty needle represents an inactive search and has no overlap
        // or history to load.
        if (window.needle_len == 0) {
            *out = nullptr;
            return true;
        }

        // First up, add enough pages to cover the active area.
        size_t rem = list->rows;
        PageList::Node *node_ = list->pages.last;
        PageList::Node *last_node = nullptr;
        while (node_ != nullptr) {
            PageList::Node *node = node_;

            {
                SlidingWindow::AppendResult appended;
                if (!window.append(node, &appended)) return false;
                (void)appended;
            }
            last_node = node;

            // If we reached our target amount, then this is the last
            // page that contains the active area. We go to the previous
            // page once more since its the first page of our required
            // overlap.
            if (rem <= node->rows()) {
                node_ = node->prev;
                break;
            }

            rem -= node->rows();

            /* Wisp: the Zig continue-expression. */
            node_ = node->prev;
        }

        // Next, add enough overlap to cover needle.len - 1 bytes (if it
        // exists) so we can cover the overlap.
        while (node_ != nullptr) {
            PageList::Node *node = node_;

            // We could be more accurate here and count bytes since the
            // last wrap but its complicated and unlikely multiple pages
            // wrap so this should be fine.
            Maybe<SlidingWindow::AppendResult> appended;
            if (!window.appendIfWrapped(node, &appended)) return false;
            if (!appended.has) break;
            if (appended.value.content_len >= window.needle_len - 1) break;

            /* Wisp: the Zig continue-expression. */
            node_ = node->prev;
        }

        // Return the last node we added to our window.
        *out = last_node;
        return true;
    }

    /// Find the next match for the needle in the active area. This returns
    /// null when there are no more matches.
    ///
    /// Wisp: ?FlattenedHighlight. false is null.
    bool next(FlattenedHighlight *out) { return window.next(out); }
};

} /* namespace search */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SEARCH_ACTIVE_HPP */
