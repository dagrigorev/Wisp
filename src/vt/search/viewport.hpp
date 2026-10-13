/* Transliterated from Ghostty src/terminal/search/viewport.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: departures from a mechanical mapping:
 *
 *   - `../../quirks.zig` is not ported in Wisp. The only thing this file
 *     uses from it is `inlineAssert`, which is `std.debug.assert` with an
 *     `inline` call modifier; it maps to plain `assert` from <assert.h>,
 *     as everywhere else in this port. Nothing else from quirks.zig is
 *     pulled in.
 *
 *   - `Allocator.Error!T` is a bool return plus an out-parameter, as in
 *     the sibling sliding_window.hpp; false is OutOfMemory.
 *
 *   - `?T` is `Maybe<T>`.
 *
 *   - `needle()` returns a `[]const u8`, so it is a pointer plus an
 *     out-length, per the port's slice mapping.
 *
 *   - `entries.shrinkRetainingCapacity(i)` has no counterpart in the
 *     port's `zigstd::ArrayListUnmanaged`. Zig's shrinkRetainingCapacity
 *     only asserts `new_len <= items.len` and assigns the length for a
 *     non-sentinel list, so it is written out as that assignment here.
 *     (The array list is shared with other files and was not modified.)
 *
 *   - `Terminal.zig` is imported upstream only for the test blocks, so it
 *     is included by the test file rather than by this header.
 */

#pragma once
#ifndef WISP_VT_SEARCH_VIEWPORT_HPP
#define WISP_VT_SEARCH_VIEWPORT_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../../zigstd/array_list.hpp"
#include "../highlight.hpp"
#include "../page_list.hpp"
#include "../point.hpp"
#include "../size.hpp"
#include "sliding_window.hpp"

namespace wisp {
namespace vt {
namespace search {

/// Searches for a substring within the viewport of a PageList.
///
/// This contains logic to efficiently detect when the viewport changes
/// and only re-search when necessary.
///
/// The specialization for "viewport" is because the viewport is the
/// only part of the search where the user can actively see the results,
/// usually. In that case, it is more efficient to re-search only the
/// viewport rather than store all the results for the entire screen.
///
/// Note that this searches all the pages that viewport covers, so
/// this can include extra matches outside the viewport if the data
/// lives in the same page.
struct ViewportSearch {
    typedef highlight::Flattened FlattenedHighlight;

    /// Viewport fingerprint so we can detect when the viewport moves.
    struct Fingerprint {
        struct Entry {
            PageList::Node *node;
            uint64_t serial;

            Entry() : node(nullptr), serial(0) {}
        };

        /// The node addresses and generations that make up the viewport. We
        /// can't safely dereference cached node pointers because the nodes may
        /// be invalid, so equality only compares these captured values.
        zigstd::ArrayListUnmanaged<Entry> entries; /* = .empty */

        Fingerprint() : entries() {}

        void deinit(zigstd::Allocator alloc) { entries.deinit(alloc); }

        void clearAndRetainCapacity() { entries.clearRetainingCapacity(); }

        /// Update this fingerprint to match the current viewport.
        ///
        /// Returns true if the viewport changed. The viewport can have at
        /// most one entry per visible row, so we reserve that upper bound
        /// before mutating the retained buffer. This lets us rebuild in one
        /// pass while still preserving the old fingerprint if allocation
        /// fails.
        ///
        /// Wisp: Allocator.Error!bool. A false return is OutOfMemory; the
        /// upstream bool result is in `*out`.
        bool update(zigstd::Allocator alloc, PageList *pages, bool *out) {
            if (!entries.ensureTotalCapacity(alloc, pages->rows)) return false;

            const size_t old_len = entries.items_len;
            bool changed = false;

            size_t i = 0;
            PageList::PageIterator it =
                pages->pageIterator(PageList::Direction::right_down, point::Point::viewport(),
                                    Maybe<point::Point>());
            PageList::Chunk chunk;
            for (; it.next(&chunk); i += 1) {
                Entry entry;
                entry.node = chunk.node;
                entry.serial = chunk.node->serial;
                if (!changed) {
                    changed = i >= old_len || entries.items[i].node != entry.node ||
                              entries.items[i].serial != entry.serial;
                }

                if (i < entries.items_len) {
                    entries.items[i] = entry;
                } else {
                    entries.appendAssumeCapacity(entry);
                }
            }

            /* Wisp: entries.shrinkRetainingCapacity(i) — see header note. */
            assert(i <= entries.items_len);
            entries.items_len = i;
            *out = changed || i != old_len;
            return true;
        }
    };

    SlidingWindow window;
    Fingerprint fingerprint; /* = .{} */

    /// If this is null, then active dirty tracking is disabled and if the
    /// viewport overlaps the active area we always re-search. If this is
    /// non-null, then we only re-search if the active area is dirty. Dirty
    /// marking is up to the caller.
    Maybe<bool> active_dirty;

    ViewportSearch() : window(), fingerprint(), active_dirty() {}

    /* Wisp: Allocator.Error!ViewportSearch. false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, const uint8_t *needle_unowned,
                     size_t needle_unowned_len, ViewportSearch *out) {
        // We just do a forward search since the viewport is usually
        // pretty small so search results are instant anyways. This avoids
        // a small amount of work to reverse things.
        SlidingWindow window;
        if (!SlidingWindow::init(alloc, SlidingWindow::Direction::forward, needle_unowned,
                                 needle_unowned_len, &window))
            return false;
        /* Wisp: errdefer window.deinit() — nothing below can fail. */
        out->window = window;
        out->fingerprint = Fingerprint();
        out->active_dirty = Maybe<bool>::none();
        return true;
    }

    void deinit() {
        fingerprint.deinit(window.alloc);
        window.deinit();
    }

    /// Reset our fingerprint and results so that the next update will
    /// always re-search.
    void reset() {
        fingerprint.clearAndRetainCapacity();
        window.clearAndRetainCapacity();
    }

    /// The needle that this search is using.
    ///
    /// Wisp: []const u8 is a pointer plus an out-length.
    const uint8_t *needle(size_t *out_len) const {
        assert(window.direction == SlidingWindow::Direction::forward);
        *out_len = window.needle_len;
        return window.needle;
    }

    /// Update the sliding window to reflect the current viewport. This
    /// will do nothing if the viewport hasn't changed since the last
    /// search.
    ///
    /// The PageList must be safe to read throughout the lifetime of this
    /// function.
    ///
    /// Returns true if the viewport changed and a re-search is needed.
    /// Returns false if the viewport is unchanged.
    ///
    /// Wisp: Allocator.Error!bool. A false return is OutOfMemory; the
    /// upstream bool result is in `*out`.
    bool update(PageList *list, bool *out) {
        // See if our viewport changed. We retain the fingerprint buffer so
        // unchanged viewports don't allocate and changed viewports usually
        // reuse the previous capacity.
        Fingerprint *fingerprint_ = &fingerprint;
        bool fp_changed = false;
        if (!fingerprint_->update(window.alloc, list, &fp_changed)) return false;
        if (!fp_changed) {
            /* Wisp: `match: { ... }` — `match_break` is `break :match`. */
            bool match_break = false;
            do {
                // Determine if we need to check if we overlap the active area.
                // If we have dirty tracking on we also set it to false here.
                bool check_active;
                /* Wisp: `active: { ... }` */
                if (!active_dirty.has) {
                    check_active = true;
                } else if (!active_dirty.value) {
                    check_active = false;
                } else {
                    active_dirty = Maybe<bool>(false);
                    check_active = true;
                }

                if (check_active) {
                    // If our fingerprint contains the active area, then we always
                    // re-search since the active area is mutable.
                    const PageList::Pin active_tl = list->getTopLeft(point::Tag::active);
                    const PageList::Pin active_br =
                        list->getBottomRight(point::Tag::active).value;

                    // If our viewport contains the start or end of the active area,
                    // we are in the active area. We purposely do this first because
                    // our viewport is always larger than the active area.
                    for (size_t ei = 0; ei < fingerprint_->entries.items_len; ei++) {
                        const Fingerprint::Entry entry = fingerprint_->entries.items[ei];
                        if (entry.node == active_tl.node) {
                            match_break = true;
                            break;
                        }
                        if (entry.node == active_br.node) {
                            match_break = true;
                            break;
                        }
                    }
                    if (match_break) break;
                }

                // No change
                *out = false;
                return true;
            } while (0);
        }

        // If our active area was set as dirty, we always unset it here
        // because we're re-searching now.
        if (active_dirty.has) active_dirty.value = false;

        // Clear our previous sliding window
        window.clearAndRetainCapacity();

        // Add enough overlap to cover needle.len - 1 bytes (if it
        // exists) so we can cover the overlap. We only do this for the
        // soft-wrapped prior pages.
        /* Wisp: `-|` saturating subtraction. */
        const size_t overlap_len = window.needle_len > 1 ? window.needle_len - 1 : 0;
        Fingerprint::Entry *entries = fingerprint_->entries.items;
        const size_t entries_len = fingerprint_->entries.items_len;
        PageList::Node *node_ = entries[0].node->prev;
        size_t added = 0;
        for (; node_ != nullptr; node_ = node_->prev) {
            PageList::Node *node = node_;
            // We could be more accurate here and count bytes since the
            // last wrap but its complicated and unlikely multiple pages
            // wrap so this should be fine.
            Maybe<SlidingWindow::AppendResult> appended_;
            if (!window.appendIfWrapped(node, &appended_)) return false;
            if (!appended_.has) break;
            const SlidingWindow::AppendResult appended = appended_.value;
            added += appended.content_len;
            if (added >= overlap_len) break;
        }

        // We can use our fingerprint nodes to initialize our sliding
        // window, since we already traversed the viewport once.
        /* Wisp: upstream is `= undefined` and relies on entries_len >= 1, since
         * the viewport always covers at least one row; if it were 0 the
         * trailing block below would read it unset. Value initialized rather
         * than left indeterminate -- strictly more defined than upstream, and
         * identical whenever that assumption holds (MSVC C4701). */
        SlidingWindow::AppendResult end_append = SlidingWindow::AppendResult();
        for (size_t ei = 0; ei < entries_len; ei++) {
            if (!window.append(entries[ei].node, &end_append)) return false;
        }

        // Add any trailing overlap as well.
        /* Wisp: `trailing: { ... }` */
        do {
            PageList::Node *end = entries[entries_len - 1].node;
            if (!end_append.last_row_wrapped) break;

            node_ = end->next;
            added = 0;
            for (; node_ != nullptr; node_ = node_->next) {
                PageList::Node *node = node_;
                SlidingWindow::AppendResult appended;
                if (!window.append(node, &appended)) return false;
                added += appended.content_len;
                if (added >= overlap_len) break;

                // If this row doesn't wrap, then we can quit
                if (!appended.last_row_wrapped) break;
            }
        } while (0);

        *out = true;
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

#endif /* WISP_VT_SEARCH_VIEWPORT_HPP */
