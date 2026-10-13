/* Transliterated from Ghostty src/terminal/search/terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: departures from a mechanical mapping:
 *
 *   - `Allocator.Error!T` is a bool return plus an out-parameter, as in the
 *     sibling screen.hpp / viewport.hpp / sliding_window.hpp; false is
 *     OutOfMemory.
 *
 *   - `?T` is `Maybe<T>`; `?*T` is a plain nullable pointer.
 *
 *   - `needle()` returns a `[]const u8`, so it is a pointer plus an
 *     out-length, per the port's slice mapping.
 *
 *   - `viewportMatches()` returns `[]const FlattenedHighlight`, which is
 *     likewise a pointer plus an out-length. Unlike ScreenSearch.matches
 *     the storage here is owned by the search and only borrowed by the
 *     caller, so the slice shape is kept rather than filling a vector.
 *
 *   - `std.EnumMap(ScreenSet.Key, V)` is `KeyMap<V>` below: an array indexed
 *     by key plus a presence flag, the same shape `ScreenSet` itself already
 *     uses for `std.EnumMap(Key, *Screen)` and apc.hpp uses for its
 *     EnumMap/EnumSet. Its iterator skips absent keys, as Zig's does, and
 *     yields a pointer to the value, as Zig's does.
 *
 *   - `t.screens.all.iterator()` iterates an `std.EnumMap(Key, *Screen)`,
 *     whose ported form is the plain `Screen *all[key_count]` array. The
 *     loop therefore walks the array and skips null slots, which is exactly
 *     what the EnumMap iterator skipping unset keys does.
 *
 *   - `self.screens.put(key, screen_search)` would be a copy of a struct
 *     holding two `std::vector`s. `ScreenSearch` has no public move and a
 *     copy would duplicate (and later double-free) that storage, so the
 *     searcher is initialized directly into its map slot and the key is
 *     marked present only on success. Nothing observable differs: upstream
 *     also stores the freshly initialized value and leaves the key absent
 *     when `init` fails.
 *
 *   - `std.ArrayList(FlattenedHighlight)` is a `std::vector`, matching
 *     screen.hpp. `items.len` is `size()`, `clearRetainingCapacity()` is
 *     `clear()` and `deinit(alloc)` is `clear()` + `shrink_to_fit()`.
 *     `highlight::Flattened::clone()` takes no allocator and cannot fail,
 *     and `append` cannot report OutOfMemory in this port, so the
 *     `errdefer` inside `viewportMatches` has no counterpart and the bool
 *     error channel there is always true. It is kept so the call shape
 *     matches upstream's `Allocator.Error!`.
 *
 *   - `ScreenSearch.tick()` is `TickError!void` upstream and
 *     `TickError tick()` here, with `TickError::none` for success; the
 *     `if (...) |_| ... else |err|` becomes a comparison plus a switch.
 *
 *   - `log.warn` is dropped; there is no logging in the port. Every site is
 *     marked with the comment upstream logged.
 *
 *   - `errdefer vp.deinit()` in `init` has no counterpart: nothing between
 *     the ViewportSearch init and the return can fail.
 *
 *   - `flattened.chunks.slice()` is a MultiArrayList slice upstream and one
 *     `std::vector<Flattened::Chunk>` here, so `hl_chunks.get(i)` is
 *     `chunks[i]`.
 */

#pragma once
#ifndef WISP_VT_SEARCH_TERMINAL_HPP
#define WISP_VT_SEARCH_TERMINAL_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "../highlight.hpp"
#include "../page_list.hpp"
#include "../point.hpp"
#include "../screen_set.hpp"
#include "../terminal.hpp"
#include "screen.hpp"
#include "viewport.hpp"

namespace wisp {
namespace vt {
namespace search {

/// Searches for a needle within an entire Terminal, orchestrating a
/// ScreenSearch per live screen plus a viewport search for the active
/// screen.
///
/// This reconciles per-screen searchers against the terminal's active
/// ScreenSet, retains results across primary/alt screen switches, and
/// recovers from resize/reflow, resets, scrollback pruning, etc.
///
/// The main ownership contract is that this is safe to call concurrently
/// with Terminal IO as long as you're not calling a function that takes
/// a terminal as an argument. Each function has more specific details about
/// its behavior.
struct TerminalSearch {
    typedef highlight::Flattened FlattenedHighlight;

    /* Wisp: std.EnumMap(ScreenSet.Key, V). See the header comment. */
    template <typename V> struct KeyMap {
        V values[ScreenSet::key_count];
        bool present[ScreenSet::key_count];

        KeyMap() {
            for (size_t i = 0; i < ScreenSet::key_count; i++) {
                values[i] = V();
                present[i] = false;
            }
        }

        bool contains(ScreenSet::Key key) const { return present[(size_t)key]; }

        V *getPtr(ScreenSet::Key key) {
            return present[(size_t)key] ? &values[(size_t)key] : nullptr;
        }

        const V *getPtr(ScreenSet::Key key) const {
            return present[(size_t)key] ? &values[(size_t)key] : nullptr;
        }

        void put(ScreenSet::Key key, const V &value) {
            values[(size_t)key] = value;
            present[(size_t)key] = true;
        }

        /* Wisp: the slot for a key that is about to be initialized in place,
         * plus the explicit presence mark. See the header comment. */
        V *slot(ScreenSet::Key key) { return &values[(size_t)key]; }
        void markPresent(ScreenSet::Key key) { present[(size_t)key] = true; }

        void remove(ScreenSet::Key key) { present[(size_t)key] = false; }

        struct Entry {
            ScreenSet::Key key;
            V *value;

            Entry() : key(ScreenSet::Key::primary), value(nullptr) {}
        };

        struct Iterator {
            KeyMap *map;
            size_t i;

            bool next(Entry *out) {
                while (i < ScreenSet::key_count) {
                    const size_t j = i;
                    i += 1;
                    if (!map->present[j]) continue;
                    out->key = (ScreenSet::Key)j;
                    out->value = &map->values[j];
                    return true;
                }
                return false;
            }
        };

        Iterator iterator() {
            Iterator it = {this, 0};
            return it;
        }
    };

    /// Allocator used for all search state.
    zigstd::Allocator alloc;

    /// Active viewport search for the active screen.
    ViewportSearch viewport;

    /// The searchers for all the screens.
    KeyMap<ScreenSearch> screens;

    /// ScreenSet generations captured when each searcher was initialized.
    /// Allocators may reuse a destroyed Screen address, so pointer equality
    /// alone cannot distinguish replacement screens from stale handles.
    KeyMap<size_t> screen_generations;

    /// The active screen key as of the last feed. All single-screen
    /// reads (total matches, selection, etc.) resolve against this.
    ScreenSet::Key active_key;

    /// True when the cached viewport matches are stale and must be
    /// recollected from the viewport searcher on the next
    /// `viewportMatches` call.
    bool stale_viewport_matches;

    /// Cached viewport matches. The viewport search's sliding window
    /// drains on read, so results are collected once per viewport
    /// change and cached here.
    std::vector<FlattenedHighlight> viewport_matches;

    /// Overall status of the search. See `status`.
    enum class Status {
        /// `tick` can make progress without terminal access.
        running,

        /// Blocked until the next `feed`. This is also the initial
        /// state, since a search that has never been fed has never
        /// seen the terminal.
        feed_required,

        /// Caught up with the terminal state as of the last feed.
        complete,
    };

    /// Viewport scroll behavior applied by `select` when a match
    /// becomes selected.
    enum class SelectScroll {
        /// Scroll so the match is visible, only if it is not already.
        if_needed,

        /// Never scroll.
        none,
    };

    TerminalSearch()
        : alloc(), viewport(), screens(), screen_generations(),
          active_key(ScreenSet::Key::primary), stale_viewport_matches(true), viewport_matches() {}

    /// Initialize a search for the given needle. The needle is copied.
    ///
    /// This doesn't read any terminal state. The first `feed` does.
    ///
    /// Wisp: Allocator.Error!TerminalSearch. false is OutOfMemory.
    static bool init(zigstd::Allocator alloc, const uint8_t *needle_unowned,
                     size_t needle_unowned_len, TerminalSearch *out) {
        ViewportSearch vp;
        if (!ViewportSearch::init(alloc, needle_unowned, needle_unowned_len, &vp)) return false;
        /* Wisp: errdefer vp.deinit() — nothing below can fail. */

        // We use dirty tracking for active area changes. Start with it
        // dirty so the first change is re-searched.
        vp.active_dirty = Maybe<bool>(true);

        out->alloc = alloc;
        out->viewport = vp;
        out->screens = KeyMap<ScreenSearch>();
        out->screen_generations = KeyMap<size_t>();
        out->active_key = ScreenSet::Key::primary;
        out->stale_viewport_matches = true;
        /* .viewport_matches = .empty */
        return true;
    }

    /// Release all state. The terminal must be the same one given to
    /// every other call, or null if the terminal has already been
    /// deinitialized. When the terminal is alive this releases tracked
    /// pins held within it. When it is null, those pins died with the
    /// terminal's page storage, so only search-owned memory is freed.
    void deinit(Terminal *t_) {
        clearViewportMatches();
        /* Wisp: viewport_matches.deinit(self.alloc) */
        viewport_matches.clear();
        viewport_matches.shrink_to_fit();
        viewport.deinit();
        KeyMap<ScreenSearch>::Iterator it = screens.iterator();
        KeyMap<ScreenSearch>::Entry entry;
        while (it.next(&entry)) {
            const bool valid = (t_ != nullptr) ? screenIsValid(&t_->screens, entry.key, entry.value)
                                               : false;
            if (valid) {
                entry.value->deinit();
            } else {
                entry.value->deinitScreenInvalid();
            }
        }
    }

private:
    /* Wisp: upstream's non-`pub` functions. */
    bool screenIsValid(const ScreenSet *screens_, ScreenSet::Key key,
                       const ScreenSearch *search) const {
        const size_t *generation_ = screen_generations.getPtr(key);
        if (generation_ == nullptr) return false;
        const size_t generation = *generation_;
        if (generation != screens_->generation(key)) return false;
        Screen *const actual = screens_->get(key);
        if (actual == nullptr) return false;
        return actual == search->screen;
    }

public:
    /// The needle that this search is using, borrowed.
    ///
    /// Wisp: []const u8 is a pointer plus an out-length.
    const uint8_t *needle(size_t *out_len) const { return viewport.needle(out_len); }

    /// The searcher for the active screen as of the last feed. Null if
    /// the search has never been fed (or the screen failed to
    /// initialize).
    ScreenSearch *activeScreenSearch() { return screens.getPtr(active_key); }

    /// Returns true if all searches on all screens are complete.
    bool isComplete() {
        KeyMap<ScreenSearch>::Iterator it = screens.iterator();
        KeyMap<ScreenSearch>::Entry entry;
        while (it.next(&entry)) {
            if (!ScreenSearch::isComplete(entry.value->state)) return false;
        }

        return true;
    }

    /// The overall search status derived from the per-screen search
    /// states.
    ///
    /// Unlike `isComplete`, a search that has never been fed reports
    /// `feed_required` rather than `complete`. An empty screen map
    /// means the search has never seen the terminal, so reporting it
    /// as complete would be technically true but useless to a caller
    /// deciding what to do next.
    Status status() {
        bool saw_any = false;
        Status result = Status::complete;
        KeyMap<ScreenSearch>::Iterator it = screens.iterator();
        KeyMap<ScreenSearch>::Entry entry;
        while (it.next(&entry)) {
            saw_any = true;
            switch (entry.value->state) {
            // Progress is possible without a feed.
            case ScreenSearch::State::active:
            case ScreenSearch::State::history: return Status::running;

            // Blocked until fed.
            case ScreenSearch::State::history_feed: result = Status::feed_required; break;

            case ScreenSearch::State::complete: break;
            }
        }

        if (!saw_any) return Status::feed_required;
        return result;
    }

    enum class Tick {
        /// All searches are complete.
        complete,

        /// Progress was made on at least one screen.
        progress,

        /// All incomplete searches are blocked on feed.
        blocked,
    };

    /// Tick the search forward as much as possible without reading
    /// any terminal state, so this is safe to call concurrently with
    /// terminal IO. Returns the overall tick progress.
    Tick tick() {
        Tick result = Tick::complete;
        KeyMap<ScreenSearch>::Iterator it = screens.iterator();
        KeyMap<ScreenSearch>::Entry entry;
        while (it.next(&entry)) {
            const ScreenSearch::TickError err = entry.value->tick();
            if (err == ScreenSearch::TickError::none) {
                result = Tick::progress;
            } else {
                switch (err) {
                case ScreenSearch::TickError::none: break;

                // Ignore... nothing we can do.
                // log.warn("error ticking screen search key={} err={}")
                case ScreenSearch::TickError::OutOfMemory: break;

                // Ignore, good for us. State remains whatever it is.
                case ScreenSearch::TickError::SearchComplete: break;

                // Ignore, too, progressed
                case ScreenSearch::TickError::FeedRequired:
                    switch (result) {
                    // If we think we're complete, we're not because we're
                    // blocked now (nothing made progress).
                    case Tick::complete: result = Tick::blocked; break;

                    // If we made some progress, we remain in progress
                    // since blocked means no progress at all.
                    case Tick::progress: break;

                    // If we're blocked already then we remain blocked.
                    case Tick::blocked: break;
                    }
                    break;
                }
            }
        }

        return result;
    }

    /// Read the terminal to update any search state that requires it,
    /// such as reconciling screens, feeding more data to the
    /// searchers, and detecting viewport changes.
    ///
    /// This reads the terminal, so the caller must ensure the terminal
    /// isn't modified for the duration of this call (e.g. by holding a
    /// lock). Feeding is also the only way the search learns about
    /// terminal changes, so callers should feed periodically while a
    /// search is in use, even after it reports complete.
    ///
    /// `active_dirty` should be true when the active area may have
    /// changed since the last feed, which triggers a re-scan of the
    /// active area. Callers that know when the active area changes
    /// (e.g. Ghostty's renderer-maintained dirty flag) can pass false
    /// to skip the re-scan. Callers without that knowledge should
    /// always pass true. That is always correct, just slightly more
    /// work, and the active area search is cheap by design.
    void feed(Terminal *t, bool active_dirty) {
        const zigstd::Allocator alloc_ = alloc;

        // Update our active screen
        if (t->screens.active_key != active_key) {
            active_key = t->screens.active_key;
        }

        // Reconcile our screens with the terminal screens. Remove
        // searchers for screens that no longer exist and add searchers
        // for screens that do exist but we don't have yet.
        {
            // Remove screens we have that no longer exist or changed.
            KeyMap<ScreenSearch>::Iterator it = screens.iterator();
            KeyMap<ScreenSearch>::Entry entry;
            while (it.next(&entry)) {
                const bool remove = !screenIsValid(&t->screens, entry.key, entry.value);

                if (remove) {
                    entry.value->deinitScreenInvalid();
                    screens.remove(entry.key);
                    screen_generations.remove(entry.key);
                }
            }
        }
        {
            // Add screens that exist but we don't have yet.
            /* Wisp: t.screens.all.iterator() over std.EnumMap(Key, *Screen);
             * the ported ScreenSet holds a plain array, and a null slot is an
             * unset key, which the EnumMap iterator skips. */
            for (size_t i = 0; i < ScreenSet::key_count; i++) {
                Screen *const screen = t->screens.all[i];
                if (screen == nullptr) continue;
                const ScreenSet::Key key = (ScreenSet::Key)i;
                if (screens.contains(key)) continue;
                size_t needle_len = 0;
                const uint8_t *const needle_ptr = viewport.needle(&needle_len);
                /* Wisp: initialized into the map slot rather than copied in.
                 * See the header comment. */
                ScreenSearch *const screen_search = screens.slot(key);
                if (!ScreenSearch::init(alloc_, screen, needle_ptr, needle_len, screen_search)) {
                    // OOM is probably going to sink the entire ship but
                    // we can just ignore it and wait on the next
                    // reconciliation to try again.
                    // log.warn("error initializing screen search for key={} err={}")
                    continue;
                }
                screens.markPresent(key);
                screen_generations.put(key, t->screens.generation(key));
            }
        }

        // The caller told us the active area may have changed, so mark
        // our viewport searcher dirty (forcing a re-search) and reload
        // the active area for the active screen.
        if (active_dirty) {
            // Mark our viewport dirty so it researches the active
            viewport.active_dirty = Maybe<bool>(true);

            // Reload our active area for our active screen
            if (ScreenSearch *const screen_search = screens.getPtr(t->screens.active_key)) {
                if (!screen_search->reloadActive()) {
                    // log.warn("error reloading active area for screen key={} err={}")
                }
            }
        }

        // Check our viewport for changes.
        {
            bool updated = false;
            if (viewport.update(&t->screens.active->pages, &updated)) {
                if (updated) stale_viewport_matches = true;
            } else {
                // log.warn("error updating viewport search err={}")
            }
        }

        // Feed data
        KeyMap<ScreenSearch>::Iterator it = screens.iterator();
        KeyMap<ScreenSearch>::Entry entry;
        while (it.next(&entry)) {
            if (ScreenSearch::needsFeed(entry.value->state)) {
                if (!entry.value->feed()) {
                    // log.warn("error feeding screen search key={} err={}")
                }
            }
        }
    }

    /// Return the matches on the pages covering the viewport, as of
    /// the last feed. Note this can include matches slightly outside
    /// the visible viewport when they share a page with it.
    ///
    /// The results are cached, since the underlying viewport search
    /// drains as it is read. Matches are collected once after a feed
    /// notices a viewport change and the cached results are returned
    /// otherwise. The returned slice is owned by the search and valid
    /// until the next call to this function, the next feed, or deinit.
    ///
    /// This doesn't read any terminal state, so it is safe to call
    /// concurrently with terminal IO.
    ///
    /// Wisp: Allocator.Error![]const FlattenedHighlight. The slice is a
    /// pointer plus an out-length; the bool return is OutOfMemory and is
    /// always true (see the header comment).
    bool viewportMatches(const FlattenedHighlight **out_ptr, size_t *out_len) {
        if (!stale_viewport_matches) {
            *out_ptr = viewport_matches.empty() ? nullptr : &viewport_matches[0];
            *out_len = viewport_matches.size();
            return true;
        }

        // We always mark the cache fresh, even if collection fails
        // below: a failed collection isn't retried until the next feed
        // marks the cache stale again.
        stale_viewport_matches = false;

        clearViewportMatches();
        /* Wisp: the errdefer that clears the matches and resets the viewport
         * has no counterpart; nothing below can fail. See the header
         * comment. */
        FlattenedHighlight hl;
        while (viewport.next(&hl)) {
            FlattenedHighlight hl_cloned = hl.clone();
            viewport_matches.push_back(hl_cloned);
        }

        *out_ptr = viewport_matches.empty() ? nullptr : &viewport_matches[0];
        *out_len = viewport_matches.size();
        return true;
    }

private:
    void clearViewportMatches() {
        for (size_t i = 0; i < viewport_matches.size(); i++) viewport_matches[i].deinit();
        viewport_matches.clear();
    }

public:
    /// Select the next or previous search result on the active screen,
    /// wrapping around at the ends, and optionally scrolling the
    /// viewport so the newly selected match is visible.
    ///
    /// This feeds first so the selection always works against current
    /// terminal state, making it safe to call at any time relative to
    /// feeds. Like `feed`, this reads (and possibly scrolls) the
    /// terminal, so the caller must ensure the terminal isn't
    /// otherwise being used for the duration of this call.
    ///
    /// Returns true if a match is selected after the operation, or
    /// false if there are no matches to select.
    ///
    /// Wisp: Allocator.Error!bool. A false return is OutOfMemory; upstream's
    /// value is in `*out`.
    bool select(Terminal *t, ScreenSearch::Select to, SelectScroll scroll, bool *out) {
        *out = false;

        // A screen can be removed or replaced between feeds. Reconcile
        // while holding the terminal lock before touching any
        // ScreenSearch pins.
        feed(t, false);
        ScreenSearch *const screen_search = screens.getPtr(active_key);
        if (screen_search == nullptr) return true;

        // Make the selection. Ignore the result because we don't
        // care if the selection didn't change.
        {
            bool discard = false;
            if (!screen_search->select(to, &discard)) return false;
        }

        // Grab our match if we have one. If we don't have a selection
        // then there was nothing to select.
        FlattenedHighlight flattened;
        if (!screen_search->selectedMatch(&flattened)) return true;

        switch (scroll) {
        case SelectScroll::none: *out = true; return true;
        case SelectScroll::if_needed: break;
        }

        // Grab the current screen and see if this match is visible within
        // the viewport already. If it is, we do nothing.
        Screen *const screen = t->screens.get(active_key);
        if (screen == nullptr) {
            *out = true;
            return true;
        }

        // Grab the viewport. Viewports and selections are usually small
        // so this check isn't very expensive, despite appearing O(N^2),
        // both Ns are usually equal to 1.
        PageList::PageIterator it = screen->pages.pageIterator(
            PageList::Direction::right_down, point::Point::viewport(), Maybe<point::Point>());
        const std::vector<FlattenedHighlight::Chunk> &hl_chunks = flattened.chunks;
        PageList::Chunk chunk;
        while (it.next(&chunk)) {
            for (size_t i = 0; i < hl_chunks.size(); i++) {
                const FlattenedHighlight::Chunk hl_chunk = hl_chunks[i];
                if (chunk.overlaps(PageList::Chunk(hl_chunk.node, hl_chunk.start, hl_chunk.end))) {
                    *out = true;
                    return true;
                }
            }
        }

        screen->scroll(Screen::Scroll::pinAt(flattened.startPin()));
        *out = true;
        return true;
    }
};

} /* namespace search */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SEARCH_TERMINAL_HPP */
