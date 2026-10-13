/* Transliterated from Ghostty src/terminal/search/screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: departures from a mechanical mapping:
 *
 *   - `Allocator.Error!T` is a bool return plus an out-parameter, as in
 *     sliding_window.hpp / active.hpp / pagelist.hpp; false is OutOfMemory.
 *
 *   - `TickError = Allocator.Error || error{FeedRequired, SearchComplete}`
 *     is `enum class TickError { none, OutOfMemory, FeedRequired,
 *     SearchComplete }`, returned by value. `none` is upstream's success.
 *
 *   - `?FlattenedHighlight` returns (matchAt, selectedMatch) are a bool
 *     return with the value in an out-parameter; false is upstream's null.
 *
 *   - `std.ArrayList(FlattenedHighlight)` is a `std::vector`, matching the
 *     choice already made by the ported highlight.zig/sliding_window.hpp.
 *     Consequently:
 *       * `items.len` is `size()`, `clearRetainingCapacity()` is `clear()`,
 *         `shrinkAndFree(alloc, i)` is `resize(i)` + `shrink_to_fit()`,
 *         `orderedRemove(i)` is `erase(begin() + i)` and
 *         `replaceRangeAssumeCapacity(0, i, &.{})` is `erase(begin(),
 *         begin() + i)`.
 *       * every `errdefer` that frees partially built result lists has no
 *         counterpart: the vector's destructor already does strictly more.
 *         The two upstream tripwire tests that exercise those errdefers are
 *         still ported, and still return OutOfMemory at the same two points.
 *       * `highlight::Flattened::deinit()` and `clone()` take no allocator
 *         (the chunk vector owns its storage), so `hl.deinit(alloc)` is
 *         `hl.deinit()` and `try hl.clone(alloc)` is `hl.clone()`, which
 *         cannot fail. The `alloc` local is retained where upstream has it
 *         only when something else still needs it.
 *
 *   - `matches(alloc) -> ![]FlattenedHighlight` fills a
 *     `std::vector<FlattenedHighlight>` out-parameter instead of returning
 *     caller-owned memory. zigstd::Allocator hands back raw storage without
 *     running constructors, which is not usable for a type holding a
 *     std::vector, so an owned slice could not be produced honestly. The
 *     callers' `defer alloc.free(matches)` therefore has no counterpart.
 *
 *   - `?T` fields are `Maybe<T>`; `?*T` locals stay plain nullable pointers.
 *
 *   - `defer` / `errdefer` are RAII guards. Zig runs deferred actions in
 *     reverse declaration order whether they are `defer` or `errdefer`, which
 *     is exactly C++ destruction order, so the guards are declared in
 *     upstream's order.
 *
 *   - Labeled blocks (`history:`, `active_prune:`, `fixup:`, `selected:`,
 *     `select_prev:`, `state:`, `hl:`) become `do { } while (0)` where no
 *     inner loop intervenes, and an explicit flag where upstream's
 *     `break :label` sits inside a `for`/`while` body (a C++ `break` there
 *     would only leave the loop). Each site is marked.
 *
 *   - `tripwire.module(enum {...}, ScreenSearch.reloadActive)` is a nested
 *     `reloadActive_tw` typedef, as in screen.hpp / page_list.hpp. Its error
 *     type is `AllocTw`, the port's Allocator.Error-only tripwire error.
 *
 *   - `log.info` is dropped; there is no logging in the port. Upstream
 *     ignores the error there (`_ = self.select(...) catch`), so the port
 *     ignores the bool return the same way.
 *
 *   - `comptime std.debug.runtime_safety` is `slow_runtime_safety`.
 *
 *   - `self.* = new` after `self.deinit()` is a field-by-field assignment
 *     with the two result vectors swapped rather than copied: a copy would
 *     reallocate, and the source is a local about to be destroyed. The local
 *     is left holding this search's already-emptied vectors, so nothing is
 *     freed twice.
 *
 *   - `TrackedHighlight` (highlight::Tracked) has no default constructor that
 *     initializes its two pins, and `SelectedMatch`/`HistorySearch` are held
 *     in `Maybe<T>`, which value-initializes. SelectedMatch's constructor
 *     therefore nulls both pins explicitly. Upstream never reads a
 *     `.selected` whose payload was not written, but C++ would leave the
 *     pointers indeterminate and `assert` disappears under NDEBUG, so the
 *     port initializes rather than relying on that invariant.
 */

#pragma once
#ifndef WISP_VT_SEARCH_SCREEN_HPP
#define WISP_VT_SEARCH_SCREEN_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <vector>

#include "../highlight.hpp"
#include "../page_list.hpp"
#include "../screen.hpp"
#include "../size.hpp"
#include "../tripwire.hpp"
#include "active.hpp"
#include "pagelist.hpp"
#include "sliding_window.hpp"

namespace wisp {
namespace vt {
namespace search {

/// Searches for a needle within a Screen, handling active area updates,
/// pages being pruned from the screen (e.g. scrollback limits), and more.
///
/// Unlike our lower-level searchers (like ActiveSearch and PageListSearch),
/// this will cache and store all search results so the caller can re-access
/// them as needed. This structure does this because it is intended to help
/// the caller handle the case where the Screen is changing while the user
/// is searching.
///
/// An inactive screen can continue to be searched in the background, and when
/// screen state changes, the renderer/caller can access the existing search
/// results without needing to re-search everything. This prevents a particularly
/// nasty UX where going to alt screen (e.g. neovim) and then back would
/// restart the full scrollback search.
struct ScreenSearch {
    typedef highlight::Flattened FlattenedHighlight;
    typedef highlight::Tracked TrackedHighlight;
    typedef PageList::Pin Pin;

    struct SelectedMatch {
        /// Index from the end of the match list (0 = most recent match)
        size_t idx;

        /// Tracked highlight so we can detect movement.
        TrackedHighlight highlight;

        /* Wisp: highlight::Tracked holds two raw Pin pointers and has no
         * initializing default constructor. Maybe<SelectedMatch>
         * value-initializes its payload, and upstream relies on the
         * invariant that a `.selected` payload is always written before it
         * is read; C++ would leave these indeterminate instead. */
        SelectedMatch() : idx(0), highlight(TrackedHighlight::initAssume(nullptr, nullptr)) {}

        void deinit(Screen *screen) { highlight.deinit(screen); }
    };

    /// History search state.
    struct HistorySearch {
        /// The actual searcher state.
        PageListSearch searcher;

        /// The pin for the first node that this searcher is searching from.
        /// We use this when the active area changes to find the diff between
        /// the top of the new active area and the previous start point
        /// to determine if we need to search more history.
        Pin *start_pin;

        HistorySearch() : searcher(), start_pin(nullptr) {}

        void deinit(Screen *screen) {
            searcher.deinit();
            screen->pages.untrackPin(start_pin);
        }
    };

    /// Search state machine
    enum class State {
        /// Currently searching the active area
        active,

        /// Currently searching the history area
        history,

        /// History search is waiting for more data to be fed before
        /// it can progress.
        history_feed,

        /// Search is complete given the current terminal state.
        complete,
    };

    /* Wisp: State's methods; C++ enums have none. */
    static bool isComplete(State self) {
        switch (self) {
        case State::complete: return true;
        default: return false;
        }
    }

    static bool needsFeed(State self) {
        switch (self) {
        case State::history_feed: return true;

        // Not obvious but complete search states will prune
        // stale history results on feed, and pick up pages
        // prepended below the searched frontier since the last
        // feed (incremental snapshot history restore).
        case State::complete: return true;

        default: return false;
        }
    }

    /// The screen being searched.
    Screen *screen;

    /// The active area search state
    ActiveSearch active;

    /// The history (scrollback) search state. May be null if there is
    /// no history yet.
    Maybe<HistorySearch> history;

    /// Current state of the search, a state machine.
    State state;

    /// The currently selected match, if any. As the screen contents
    /// change or get pruned, the screen search will do its best to keep
    /// this accurate.
    Maybe<SelectedMatch> selected;

    /// The results found so far. These are stored separately because history
    /// is mostly immutable once found, while active area results may
    /// change. This lets us easily reset the active area results for a
    /// re-search scenario.
    std::vector<FlattenedHighlight> history_results;
    std::vector<FlattenedHighlight> active_results;

    /// The dimensions of the screen. When this changes we need to
    /// restart the whole search, currently.
    size::CellCountInt rows;
    size::CellCountInt cols;

    ScreenSearch()
        : screen(nullptr), active(), history(), state(State::active), selected(), history_results(),
          active_results(), rows(0), cols(0) {}

    /* Wisp: the tripwire module. Upstream declares it at file scope as
     * `tripwire.module(enum {...}, ScreenSearch.reloadActive)`. */
    enum class ReloadActiveTw { history_append_new, history_append_existing };
    typedef tripwire::Module<ReloadActiveTw, AllocTw, 2> reloadActive_tw;

    // Initialize a screen search for the given screen and needle.
    /* Wisp: Allocator.Error!ScreenSearch. false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, Screen *screen, const uint8_t *needle_unowned,
                     size_t needle_unowned_len, ScreenSearch *out) {
        ScreenSearch result;
        result.screen = screen;
        result.rows = screen->pages.rows;
        result.cols = screen->pages.cols;
        if (!ActiveSearch::init(alloc, needle_unowned, needle_unowned_len, &result.active)) {
            return false;
        }
        result.history = Maybe<HistorySearch>::none();
        result.state = State::active;
        /* .active_results = .empty, .history_results = .empty */

        /* Wisp: errdefer result.deinit(); */
        struct ResultGuard {
            ScreenSearch *r;
            bool armed;
            ~ResultGuard() {
                if (armed) r->deinit();
            }
        } result_guard = {&result, true};

        // Update our initial active area state
        if (!result.reloadActive()) return false;

        result_guard.armed = false;
        result.moveTo(out);
        return true;
    }

    void deinit() { deinitInternal(true); }

    /// Release owned search state after the underlying Screen has already
    /// been destroyed. Tracked pins belonged to that Screen's PageList pool
    /// and were freed with it, so only independently owned memory is freed.
    void deinitScreenInvalid() { deinitInternal(false); }

private:
    void deinitInternal(bool screen_valid) {
        /* Wisp: `const alloc = self.allocator()` — the Flattened deinit below
         * does not take one (see the header comment), so the local is gone. */
        active.deinit();
        if (history.has) {
            HistorySearch *h = &history.value;
            if (screen_valid) {
                h->deinit(screen);
            } else {
                h->searcher.deinitListInvalid();
            }
        }
        if (screen_valid) {
            if (selected.has) selected.value.deinit(screen);
        }
        for (size_t i = 0; i < active_results.size(); i++) active_results[i].deinit();
        active_results.clear();
        active_results.shrink_to_fit();
        for (size_t i = 0; i < history_results.size(); i++) history_results[i].deinit();
        history_results.clear();
        history_results.shrink_to_fit();
    }

    /* Wisp: `self.* = new`. See the header comment: the result vectors are
     * swapped rather than copied, and the source is a local that is destroyed
     * right after holding only our already-emptied vectors. */
    void moveTo(ScreenSearch *out) {
        out->screen = screen;
        out->active = active;
        out->history = history;
        out->state = state;
        out->selected = selected;
        out->history_results.swap(history_results);
        out->active_results.swap(active_results);
        out->rows = rows;
        out->cols = cols;
    }

public:
    zigstd::Allocator allocator() const { return active.window.alloc; }

    /// The needle that this search is using.
    /* Wisp: []const u8 is a pointer plus a length out-parameter. */
    const uint8_t *needle(size_t *len) const {
        assert(active.window.direction == SlidingWindow::Direction::forward);
        *len = active.window.needle_len;
        return active.window.needle;
    }

    /// Returns the total number of matches found so far.
    size_t matchesLen() const { return active_results.size() + history_results.size(); }

private:
    /// Reinitialize cached search state after a screen resize. Reflow can
    /// replace every PageList node, so even flattened highlights that weren't
    /// selected contain invalid node pointers once either dimension changes.
    ///
    /// Returns true when the reset occurred. The replacement search has
    /// already loaded its active area through `init` in that case.
    ///
    /// Wisp: Allocator.Error!bool. A false return is OutOfMemory; upstream's
    /// value is in `*out`.
    bool resetIfDimensionsChanged(bool *out) {
        *out = false;

        if (screen->pages.rows == rows && screen->pages.cols == cols) {
            return true;
        }

        size_t needle_len = 0;
        const uint8_t *needle_ptr = needle(&needle_len);
        ScreenSearch new_search;
        if (!init(allocator(), screen, needle_ptr, needle_len, &new_search)) return false;

        deinit();
        new_search.moveTo(this);

        assert(screen->pages.rows == rows);
        assert(screen->pages.cols == cols);
        *out = true;
        return true;
    }

public:
    /// Returns all matches as an owned slice (caller must free).
    /// The matches are ordered from most recent to oldest (e.g. bottom
    /// of the screen to top of the screen).
    ///
    /// Wisp: fills `*out` rather than returning owned memory; see the header
    /// comment. The bool return is OutOfMemory and is always true.
    bool matches(std::vector<FlattenedHighlight> *out) {
        const std::vector<FlattenedHighlight> &active_results_ = active_results;
        const std::vector<FlattenedHighlight> &history_results_ = history_results;
        std::vector<FlattenedHighlight> results;
        results.resize(active_results_.size() + history_results_.size());

        // Active does a forward search, so we add the active results then
        // reverse them. There are usually not many active results so this
        // is fast enough compared to adding them in reverse order.
        assert(active.window.direction == SlidingWindow::Direction::forward);
        for (size_t i = 0; i < active_results_.size(); i++) results[i] = active_results_[i];
        std::reverse(results.begin(), results.begin() + (ptrdiff_t)active_results_.size());

        // History does a backward search, so we can just append them
        // after.
        for (size_t i = 0; i < history_results_.size(); i++) {
            results[active_results_.size() + i] = history_results_[i];
        }

        out->swap(results);
        return true;
    }

    /// Search the full screen state. This will block until the search
    /// is complete. For performance, it is recommended to use `tick` and
    /// `feed` to incrementally make progress on the search instead.
    /* Wisp: Allocator.Error!void. false is OutOfMemory. */
    bool searchAll() {
        while (true) {
            const TickError err = tick();
            switch (err) {
            case TickError::none: break;
            case TickError::OutOfMemory: return false;
            case TickError::FeedRequired:
                if (!feed()) return false;
                break;
            case TickError::SearchComplete: return true;
            }
        }
    }

    /* Wisp: Allocator.Error || error{FeedRequired, SearchComplete}. */
    enum class TickError { none, OutOfMemory, FeedRequired, SearchComplete };

    /// Make incremental progress on the search without accessing any
    /// screen state (so no lock is required).
    ///
    /// This will return error.FeedRequired if the search cannot make progress
    /// without being fed more data. In this case, the caller should call
    /// the `feed` function to provide more data to the searcher.
    ///
    /// This will return error.SearchComplete if the search is fully complete.
    /// This is to signal to the caller that it can move to a more efficient
    /// sleep/wait state until there is more work to do (e.g. new data to feed).
    TickError tick() {
        switch (state) {
        case State::active: return tickActive() ? TickError::none : TickError::OutOfMemory;
        case State::history: return tickHistory() ? TickError::none : TickError::OutOfMemory;
        case State::history_feed: return TickError::FeedRequired;
        case State::complete: return TickError::SearchComplete;
        }
        return TickError::none;
    }

    /// Feed more data to the searcher so it can continue searching. This
    /// accesses the screen state, so the caller must hold the necessary locks.
    ///
    /// Feed on a complete screen search will perform some cleanup of
    /// potentially stale history results (pruned) and reclaim some memory.
    ///
    /// If pages were prepended to the PageList since the search completed
    /// (incremental snapshot history restore), the feed picks them up and
    /// the search resumes, since "complete" only ever means caught up with
    /// the PageList as of the last feed.
    /* Wisp: Allocator.Error!void. false is OutOfMemory. */
    bool feed() {
        // Resize/reflow invalidates every flattened result, not just search
        // state that needs another history feed.
        {
            bool reset = false;
            if (!resetIfDimensionsChanged(&reset)) return false;
            (void)reset;
        }

        /* Wisp: `if (self.history) |*h| &h.searcher else { ... }`. */
        if (!history.has) {
            // No history to feed, search is complete.
            state = State::complete;
            return true;
        }
        PageListSearch *const history_searcher = &history.value.searcher;

        // Future: we may want to feed multiple pages at once here to
        // lower the frequency of lock acquisitions.
        {
            bool fed = false;
            if (!history_searcher->feed(&fed)) return false;
            if (!fed) {
                // No more data to feed, search is complete.
                state = State::complete;

                // We use this opportunity to also clean up older history
                // results that may be gone due to scrollback pruning, though.
                pruneHistory();

                return true;
            }
        }

        // Depending on our state handle where feed goes
        switch (state) {
        // If we're searching active or history, then feeding doesn't
        // change the state.
        case State::active:
        case State::history: break;

        // Feed goes back to searching history.
        case State::history_feed: state = State::history; break;

        // A complete search had exhausted the PageList as of its last
        // feed, but pages can still be prepended below that afterward
        // (incremental snapshot history restore). The history searcher's
        // tracked pin keeps its node identity through a prepend, so it
        // just discovered those older pages and loaded them. Resume
        // searching history.
        case State::complete: state = State::history; break;
        }
        return true;
    }

private:
    bool historyResultIsValid(const FlattenedHighlight *hl) const {
        /* Wisp: the MultiArrayList field slices are one vector of structs. */
        for (size_t i = 0; i < hl->chunks.size(); i++) {
            if (!screen->pages.nodeIsValid(hl->chunks[i].node, hl->chunks[i].serial)) return false;
        }

        return true;
    }

    /// Clear a selected history match if its flattened page coordinates are
    /// stale. reloadActive only inspects the selected history result, so this
    /// avoids validating every cached result on each active-area refresh.
    void validateSelectedHistory() {
        if (!selected.has) return;
        SelectedMatch *const m = &selected.value;
        const size_t active_len = active_results.size();
        if (m->idx < active_len) return;

        const size_t history_idx = m->idx - active_len;
        if (history_idx < history_results.size() &&
            historyResultIsValid(&history_results[history_idx])) {
            return;
        }

        m->deinit(screen);
        selected = Maybe<SelectedMatch>::none();
    }

    void pruneHistory() {
        // Go through our history results in order (newest to oldest) to find
        // any result that contains an invalid serial.
        size_t i = 0;
        while (i < history_results.size()) {
            FlattenedHighlight *const hl = &history_results[i];
            const uint64_t first_serial = hl->chunks[0].serial;
            if (first_serial < screen->pages.page_serial_epoch) {
                // Only a whole-list reset advances the epoch. Results are
                // newest to oldest, so this result and the entire remaining
                // suffix predate that reset. Drop them without scanning the
                // live page list for every captured chunk.
                if (selected.has) {
                    SelectedMatch *const m = &selected.value;
                    const size_t first_pruned = active_results.size() + i;
                    if (m->idx >= first_pruned) {
                        m->deinit(screen);
                        selected = Maybe<SelectedMatch>::none();
                    }
                }
                for (size_t j = i; j < history_results.size(); j++) history_results[j].deinit();
                /* Wisp: shrinkAndFree(alloc, i) */
                history_results.resize(i);
                history_results.shrink_to_fit();
                return;
            }

            // Ordinary pruning, layout changes, and replacements do not
            // advance the epoch. Validate their pointer-plus-generation pairs
            // against the live list.
            if (historyResultIsValid(hl)) {
                i += 1;
                continue;
            }

            // Only this result is known to be invalid. Older results may live
            // on unrelated pages and remain usable, so remove it individually.
            const size_t result_idx = active_results.size() + i;
            if (selected.has) {
                SelectedMatch *const m = &selected.value;
                if (m->idx == result_idx) {
                    m->deinit(screen);
                    selected = Maybe<SelectedMatch>::none();
                } else if (m->idx > result_idx) {
                    m->idx -= 1;
                }
            }

            /* Wisp: `var removed = self.history_results.orderedRemove(i);
             * removed.deinit(...)` — the element is deinit'd in place and
             * then erased, which is equivalent and avoids a copy. */
            history_results[i].deinit();
            history_results.erase(history_results.begin() + (ptrdiff_t)i);
        }
    }

    /* Wisp: Allocator.Error!void. false is OutOfMemory. */
    bool tickActive() {
        // For the active area, we consume the entire search in one go
        // because the active area is generally small.
        FlattenedHighlight hl;
        while (active.next(&hl)) {
            // If this fails, then we miss a result since `active.next()`
            // moves forward and prunes data. In the future, we may want
            // to have some more robust error handling but the only
            // scenario this would fail is OOM and we're probably in
            // deeper trouble at that point anyways.
            FlattenedHighlight hl_cloned = hl.clone();
            active_results.push_back(hl_cloned);
        }

        // We've consumed the entire active area, move to history.
        state = State::history;
        return true;
    }

    /* Wisp: Allocator.Error!void. false is OutOfMemory. */
    bool tickHistory() {
        /* Wisp: `if (self.history) |*h| h else { ... }`. */
        if (!history.has) {
            // No history to search, we're done.
            state = State::complete;
            return true;
        }
        HistorySearch *const hist = &history.value;

        // Try to consume all the loaded matches in one go, because
        // the search is generally fast for loaded data.
        FlattenedHighlight hl;
        while (hist->searcher.next(&hl)) {
            // Ignore selections that are found within the starting
            // node since those are covered by the active area search.
            if (hl.chunks[0].node == hist->start_pin->node) continue;

            // Same note as tickActive for error handling.
            FlattenedHighlight hl_cloned = hl.clone();
            history_results.push_back(hl_cloned);

            // Since history only appends to our results in reverse order,
            // we don't need to update any selected match state. The index
            // and prior results are unaffected.
        }

        // We need to be fed more data.
        state = State::history_feed;
        return true;
    }

public:
    /// Reload the active area because it has changed.
    ///
    /// Since it is very fast, this will also do the full active area
    /// search again, too. This avoids any complexity around the search
    /// state machine.
    ///
    /// The caller must hold the necessary locks to access the screen state.
    /* Wisp: Allocator.Error!void. false is OutOfMemory. */
    bool reloadActive() {
        // This check must precede all inspection of cached highlights and
        // searchers: column reflow may have freed every node they reference.
        {
            bool reset = false;
            if (!resetIfDimensionsChanged(&reset)) return false;
            if (reset) return true;
        }

        // reloadActive only inspects the selected history result, so validate
        // that result without scanning every cached match on this hot path.
        validateSelectedHistory();

        typedef reloadActive_tw tw;

        // If our selection pin became garbage it means we scrolled off
        // the end. Clear our selection and on exit of this function,
        // try to select the last match.
        /* Wisp: the `select_prev:` labeled block. */
        bool select_prev = false;
        do {
            if (!selected.has) break;
            SelectedMatch *const m = &selected.value;
            if (!m->highlight.start->garbage && !m->highlight.end->garbage) break;

            m->deinit(screen);
            selected = Maybe<SelectedMatch>::none();
            select_prev = true;
        } while (0);
        /* Wisp: defer if (select_prev) { _ = self.select(.prev) catch ... }.
         * Declared first so it runs last, matching Zig's reverse order. */
        struct SelectPrevGuard {
            ScreenSearch *self;
            const bool *select_prev;
            ~SelectPrevGuard() {
                if (*select_prev) {
                    bool discard = false;
                    /* log.info("reload failed to reset search selection") */
                    (void)self->select(Select::prev, &discard);
                }
            }
        } select_prev_guard = {this, &select_prev};
        (void)select_prev_guard;

        PageList *const list = &screen->pages;
        PageList::Node *history_node = nullptr;
        if (!active.update(list, &history_node)) return false;
        if (history_node != nullptr) {
            /* Wisp: the `history:` labeled block. */
            do {
                // We need to account for any active area growth that would
                // cause new pages to move into our history. If there are new
                // pages then we need to re-search the pages and add it to
                // our history results.

                // If our screen has no scrollback then we have no history.
                if (screen->no_scrollback) {
                    assert(!history.has);
                    break;
                }

                /* Wisp: the `state:` labeled block; null is a null pointer. */
                HistorySearch *history_ = nullptr;
                if (history.has) {
                    HistorySearch *const h = &history.value;
                    // If our start pin became garbage, it means we pruned all
                    // the way up through it, so we have no history anymore.
                    // Reset our history state.
                    if (h->start_pin->garbage) {
                        h->deinit(screen);
                        history = Maybe<HistorySearch>::none();
                        for (size_t i = 0; i < history_results.size(); i++) {
                            history_results[i].deinit();
                        }
                        history_results.clear();
                        history_ = nullptr;
                    } else {
                        history_ = h;
                    }
                }

                if (history_ == nullptr) {
                    // No history search yet, but we now have history. So let's
                    // initialize.

                    size_t needle_len = 0;
                    const uint8_t *needle_ptr = needle(&needle_len);
                    PageListSearch search;
                    if (!PageListSearch::init(allocator(), needle_ptr, needle_len, list,
                                              history_node, &search)) {
                        return false;
                    }
                    /* Wisp: errdefer search.deinit(); */
                    struct SearchGuard {
                        PageListSearch *s;
                        bool armed;
                        ~SearchGuard() {
                            if (armed) s->deinit();
                        }
                    } search_guard = {&search, true};

                    Pin *const pin = list->trackPin(Pin(history_node));
                    if (!pin) return false;
                    /* Wisp: errdefer list.untrackPin(pin) — nothing after
                     * this can fail. */

                    search_guard.armed = false;
                    HistorySearch hs;
                    hs.searcher = search;
                    hs.start_pin = pin;
                    history = Maybe<HistorySearch>(hs);

                    // We don't need to update any history since we had no history
                    // before, so we can break out of the whole conditional.
                    break;
                }
                HistorySearch *const hist = history_;

                if (hist->start_pin->node == history_node) {
                    // No change in the starting node, we're done.
                    break;
                }

                // Do a forward search from our prior node to this one. We
                // collect all the results into a new list. We ASSUME that
                // reloadActive is being called frequently enough that there isn't
                // a massive amount of history to search here.
                size_t needle_len = 0;
                const uint8_t *needle_ptr = needle(&needle_len);
                SlidingWindow window;
                if (!SlidingWindow::init(allocator(), SlidingWindow::Direction::forward, needle_ptr,
                                         needle_len, &window)) {
                    return false;
                }
                /* Wisp: defer window.deinit(); */
                struct WindowGuard {
                    SlidingWindow *w;
                    ~WindowGuard() { w->deinit(); }
                } window_guard = {&window};
                (void)window_guard;
                while (true) {
                    SlidingWindow::AppendResult discard;
                    if (!window.append(hist->start_pin->node, &discard)) return false;
                    if (hist->start_pin->node == history_node) break;
                    PageList::Node *const next = hist->start_pin->node->next;
                    if (next == nullptr) break;
                    hist->start_pin->node = next;
                }
                assert(hist->start_pin->node == history_node);

                /* Wisp: initCapacity + errdefer-free is just a vector. */
                std::vector<FlattenedHighlight> results;
                results.reserve(history_results.size());
                {
                    FlattenedHighlight hl;
                    while (window.next(&hl)) {
                        if (hl.chunks[0].node == history_node) continue;

                        FlattenedHighlight hl_cloned = hl.clone();
                        if (tw::check(ReloadActiveTw::history_append_new) != AllocTw::none) {
                            return false;
                        }
                        results.push_back(hl_cloned);
                    }
                }

                // If we have no matches then there is nothing to change
                // in our history (fast path)
                if (results.size() == 0) break;

                // The number added to our history. Needed for updating
                // our selection if we have one.
                const size_t added_len = results.size();

                // Matches! Reverse our list then append all the remaining
                // history items that didn't start on our original node.
                std::reverse(results.begin(), results.end());
                if (tw::check(ReloadActiveTw::history_append_existing) != AllocTw::none) {
                    return false;
                }
                for (size_t i = 0; i < history_results.size(); i++) {
                    results.push_back(history_results[i]);
                }
                /* Wisp: `self.history_results.deinit(alloc); self.history_results
                 * = results;` — the old elements' chunk vectors were just
                 * copied into `results`, so the originals are dropped by the
                 * swap and the local's destructor. */
                history_results.swap(results);
                for (size_t i = 0; i < results.size(); i++) results[i].deinit();
                results.clear();

                // If our prior selection was in the history area, update
                // the offset.
                /* Wisp: the `selected:` labeled block. */
                if (selected.has) {
                    SelectedMatch *const m = &selected.value;
                    do {
                        const size_t active_len = active_results.size();
                        if (m->idx < active_len) break;
                        m->idx += added_len;

                        // Moving the idx should not change our targeted result
                        // since the history is immutable.
                        if (slow_runtime_safety) {
                            const FlattenedHighlight &hl = history_results[m->idx - active_len];
                            assert(m->highlight.start->eql(hl.startPin()));
                        }
                    } while (0);
                }
            } while (0);
        } else {
            // No history node means we have no history
            if (history.has) {
                HistorySearch *const h = &history.value;
                h->deinit(screen);
                history = Maybe<HistorySearch>::none();
                for (size_t i = 0; i < history_results.size(); i++) history_results[i].deinit();
                history_results.clear();
            }

            // If we have a selection in the history area, we need to
            // move it to the end of the active area.
            /* Wisp: the `selected:` labeled block. */
            if (selected.has) {
                SelectedMatch *const m = &selected.value;
                do {
                    const size_t active_len = active_results.size();
                    if (m->idx < active_len) break;
                    m->deinit(screen);
                    selected = Maybe<SelectedMatch>::none();
                    bool discard = false;
                    /* log.info("reload failed to reset search selection") */
                    (void)select(Select::prev, &discard);
                } while (0);
            }
        }

        // Figure out if we need to fixup our selection later because
        // it was in the active area.
        const size_t old_active_len = active_results.size();
        const Maybe<size_t> old_selection_idx =
            selected.has ? Maybe<size_t>(selected.value.idx) : Maybe<size_t>::none();
        /* Wisp: the errdefer. `ok` is set true just before the single
         * success return below. */
        bool ok = false;
        struct ErrGuard {
            ScreenSearch *self;
            const bool *ok;
            const Maybe<size_t> *old_selection_idx;
            size_t old_active_len;
            ~ErrGuard() {
                if (*ok) return;
                if (!(old_selection_idx->has && old_selection_idx->value < old_active_len)) return;
                // This is the error scenario. If something fails below,
                // our active area is probably gone, so we just go back
                // to the first result because our selection can't be trusted.
                if (self->selected.has) {
                    SelectedMatch *const m = &self->selected.value;
                    m->deinit(self->screen);
                    self->selected = Maybe<SelectedMatch>::none();
                    bool discard = false;
                    /* log.info("reload failed to reset search selection") */
                    (void)self->select(Select::next, &discard);
                }
            }
        } err_guard = {this, &ok, &old_selection_idx, old_active_len};
        (void)err_guard;

        // Reset our active search results and search again.
        for (size_t i = 0; i < active_results.size(); i++) active_results[i].deinit();
        active_results.clear();
        switch (state) {
        // If we're in the active state we run a normal tick so
        // we can move into a better state.
        case State::active:
            if (!tickActive()) return false;
            break;

        // Otherwise, just tick it and move back to whatever state
        // we were in.
        default: {
            const State old_state = state;
            /* Wisp: defer self.state = old_state; */
            struct StateGuard {
                ScreenSearch *self;
                State old_state;
                ~StateGuard() { self->state = old_state; }
            } state_guard = {this, old_state};
            (void)state_guard;
            if (!tickActive()) return false;
            break;
        }
        }

        // If we have no scrollback, we need to prune any active results
        // that aren't in the actual active area. We only do this for the
        // no scrollback scenario because with scrollback we actually
        // rely on our active search searching by page to find history
        // items as well. This is all related to the fact that PageList
        // scrollback limits are discrete by page size except we special
        // case zero.
        if (screen->no_scrollback && active_results.size() > 0) {
            /* Wisp: the `active_prune:` labeled block. Upstream's
             * `break :active_prune` sits inside the `for` body, so it is a
             * flag here rather than a C++ `break`. */
            bool found_active = false;
            const Pin tl = screen->pages.getTopLeft(point::Tag::active);
            /* Wisp: `items` aliases active_results.items; the loop bound is
             * snapshotted because nothing in the body may change it. */
            const size_t items_len = active_results.size();
            for (size_t i = 0; i < items_len; i++) {
                FlattenedHighlight *const hl = &active_results[i];
                if (!tl.before(hl->endPin())) {
                    // Deinit because its going to be pruned no matter
                    // what at some point for not being in the active area.
                    hl->deinit();
                    continue;
                }

                // In the active area! Since our results are sorted
                // that means everything after this is also in the active
                // area, so we prune up to this i.
                if (i > 0) {
                    active_results.erase(active_results.begin(),
                                         active_results.begin() + (ptrdiff_t)i);
                }

                found_active = true;
                break;
            }

            // None are in the active area...
            if (!found_active) active_results.clear();
        }

        // Now we have to fixup our selection if we had one.
        /* Wisp: the `fixup:` labeled block. Its last `break :fixup` sits
         * inside the `for` body, so a flag carries the exit. */
        do {
            if (!old_selection_idx.has) break;
            const size_t old_idx = old_selection_idx.value;
            if (!selected.has) break;
            SelectedMatch *const m = &selected.value;

            // If our old selection wasn't in the active area, then we
            // need to fix up our offsets.
            if (old_idx >= old_active_len) {
                m->idx -= old_active_len;
                m->idx += active_results.size();
                break;
            }

            // We search for the matching highlight in the new active results.
            bool found = false;
            const size_t items_len = active_results.size();
            for (size_t i = 0; i < items_len; i++) {
                const highlight::Untracked untracked = active_results[i].untracked();
                if (m->highlight.start->eql(untracked.start) &&
                    m->highlight.end->eql(untracked.end)) {
                    // Found it! Update our index.
                    m->idx = active_results.size() - 1 - i;
                    found = true;
                    break;
                }
            }
            if (found) break;

            // No match, just go back to the first match.
            m->deinit(screen);
            selected = Maybe<SelectedMatch>::none();
            bool discard = false;
            /* log.info("reload failed to reset search selection") */
            (void)select(Select::next, &discard);
        } while (0);

        ok = true;
        return true;
    }

    /// Return the match at the given index in newest-to-oldest order
    /// (0 = most recent match). Returns null if the index is out of
    /// range.
    ///
    /// This does not require read/write access to the underlying screen.
    ///
    /// Wisp: ?FlattenedHighlight. false is null.
    bool matchAt(size_t idx, FlattenedHighlight *out) const {
        const size_t active_len = active_results.size();
        if (idx < active_len) {
            *out = active_results[active_len - 1 - idx];
            return true;
        }

        const size_t history_len = history_results.size();
        if (idx < active_len + history_len) {
            *out = history_results[idx - active_len];
            return true;
        }

        return false;
    }

    /// Return the selected match.
    ///
    /// This does not require read/write access to the underlying screen.
    ///
    /// Wisp: ?FlattenedHighlight. false is null.
    bool selectedMatch(FlattenedHighlight *out) const {
        if (!selected.has) return false;
        return matchAt(selected.value.idx, out);
    }

    enum class Select {
        /// Next selection, in reverse order (newest to oldest),
        /// non-wrapping.
        next,

        /// Prev selection, in forward order (oldest to newest),
        /// non-wrapping.
        prev,
    };

    /// Select the next or previous search result. This requires read/write
    /// access to the underlying screen, since we utilize tracked pins to
    /// ensure our selection sticks with contents changing.
    ///
    /// Wisp: Allocator.Error!bool. A false return is OutOfMemory; upstream's
    /// value is in `*out`.
    bool select(Select to, bool *out) {
        *out = false;

        // A resize can replace any page node, including nodes retained by
        // flattened history results. Reset before reloadActive or tracking a
        // selected result so no pre-resize pointer is ever dereferenced.
        {
            bool reset = false;
            if (!resetIfDimensionsChanged(&reset)) return false;
            (void)reset;
        }

        // Reload validates the selected history result before inspecting its
        // cached coordinates. Prune the remaining results afterward so every
        // candidate is valid before selection tracks it.
        if (!reloadActive()) return false;
        pruneHistory();

        switch (to) {
        case Select::next: return selectNext(out);
        case Select::prev: return selectPrev(out);
        }
        return true;
    }

private:
    /* Wisp: Allocator.Error!bool. false is OutOfMemory. */
    bool selectNext(bool *out) {
        *out = false;

        // Get our previous match so we can change it. If we have no
        // prior match, we have the easy task of getting the first.
        if (!selected.has) {
            // Get our highlight
            /* Wisp: the `hl:` labeled block. */
            FlattenedHighlight hl;
            if (active_results.size() > 0) {
                // Active is in forward order
                const size_t len = active_results.size();
                hl = active_results[len - 1];
            } else if (history_results.size() > 0) {
                // History is in reverse order
                hl = history_results[0];
            } else {
                // No matches at all. Can't select anything.
                *out = false;
                return true;
            }

            // Pin it so we can track any movement
            TrackedHighlight tracked = TrackedHighlight::initAssume(nullptr, nullptr);
            if (!highlight::track(hl.untracked(), screen, &tracked)) return false;
            /* Wisp: errdefer tracked.deinit(screen) — nothing after this
             * can fail. */

            // Our selection is index zero since we just started and
            // we store our selection.
            SelectedMatch m;
            m.idx = 0;
            m.highlight = tracked;
            selected = Maybe<SelectedMatch>(m);
            *out = true;
            return true;
        }
        SelectedMatch *const prev = &selected.value;

        const size_t active_len = active_results.size();
        const size_t history_len = history_results.size();
        const size_t next_idx = (prev->idx + 1 >= active_len + history_len) ? 0 : prev->idx + 1;
        const FlattenedHighlight hl = (next_idx < active_len)
                                          ? active_results[active_len - 1 - next_idx]
                                          : history_results[next_idx - active_len];

        // Pin it so we can track any movement
        TrackedHighlight tracked = TrackedHighlight::initAssume(nullptr, nullptr);
        if (!highlight::track(hl.untracked(), screen, &tracked)) return false;
        /* Wisp: errdefer tracked.deinit(screen) — nothing after this can
         * fail. */

        // Free our previous match and setup our new selection
        prev->deinit(screen);
        SelectedMatch m;
        m.idx = next_idx;
        m.highlight = tracked;
        selected = Maybe<SelectedMatch>(m);

        *out = true;
        return true;
    }

    /* Wisp: Allocator.Error!bool. false is OutOfMemory. */
    bool selectPrev(bool *out) {
        *out = false;

        // Get our previous match so we can change it. If we have no
        // prior match, we have the easy task of getting the last.
        if (!selected.has) {
            // Get our highlight (oldest match)
            /* Wisp: the `hl:` labeled block. */
            FlattenedHighlight hl;
            if (history_results.size() > 0) {
                // History is in reverse order, so last item is oldest
                const size_t len = history_results.size();
                hl = history_results[len - 1];
            } else if (active_results.size() > 0) {
                // Active is in forward order, so first item is oldest
                hl = active_results[0];
            } else {
                // No matches at all. Can't select anything.
                *out = false;
                return true;
            }

            // Pin it so we can track any movement
            TrackedHighlight tracked = TrackedHighlight::initAssume(nullptr, nullptr);
            if (!highlight::track(hl.untracked(), screen, &tracked)) return false;
            /* Wisp: errdefer tracked.deinit(screen) — nothing after this
             * can fail. */

            // Our selection is the last index since we just started
            // and we store our selection.
            const size_t active_len = active_results.size();
            const size_t history_len = history_results.size();
            SelectedMatch m;
            m.idx = active_len + history_len - 1;
            m.highlight = tracked;
            selected = Maybe<SelectedMatch>(m);
            *out = true;
            return true;
        }
        SelectedMatch *const prev = &selected.value;

        const size_t active_len = active_results.size();
        const size_t history_len = history_results.size();
        const size_t next_idx = (prev->idx != 0) ? prev->idx - 1 : active_len + history_len - 1;

        const FlattenedHighlight hl = (next_idx < active_len)
                                          ? active_results[active_len - 1 - next_idx]
                                          : history_results[next_idx - active_len];

        // Pin it so we can track any movement
        TrackedHighlight tracked = TrackedHighlight::initAssume(nullptr, nullptr);
        if (!highlight::track(hl.untracked(), screen, &tracked)) return false;
        /* Wisp: errdefer tracked.deinit(screen) — nothing after this can
         * fail. */

        // Free our previous match and setup our new selection
        prev->deinit(screen);
        SelectedMatch m;
        m.idx = next_idx;
        m.highlight = tracked;
        selected = Maybe<SelectedMatch>(m);

        *out = true;
        return true;
    }
};

} /* namespace search */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SEARCH_SCREEN_HPP */
