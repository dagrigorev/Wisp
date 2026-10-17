/* Transliterated from Ghostty src/terminal/render.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: the global mapping decisions for this file, each also marked at
 * its site below:
 *
 *   - `std.MultiArrayList(T)` is one `zigstd::ArrayListUnmanaged<T>` of
 *     structs, the mapping already used for highlight.zig's chunks and
 *     search/sliding_window.zig. Upstream's `slice().items(.field)`
 *     parallel arrays therefore become `items[i].field`.
 *   - `Allocator.Error!void` is `bool`; false is OutOfMemory.
 *   - `defer` is an RAII guard.
 *   - `?T` is `Maybe<T>`.
 *   - Nested types are defined before the types that contain them, because
 *     C++ requires a nested type to be complete at the point of use.
 *     Upstream's declaration order is otherwise preserved.
 */

#pragma once
#ifndef WISP_VT_RENDER_HPP
#define WISP_VT_RENDER_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../zigstd/allocator.hpp"
#include "../zigstd/array_list.hpp"
#include "fastmem.hpp"
#include "formatter.hpp"
#include "highlight.hpp"
#include "page.hpp"
#include "page_list.hpp"
#include "point.hpp"
#include "screen.hpp"
#include "screen_set.hpp"
#include "selection.hpp"
#include "size.hpp"
#include "style.hpp"
#include "terminal.hpp"

namespace wisp {
namespace vt {
namespace render {

/* Developer note: this is in src/terminal and not src/renderer because
 * the goal is that this remains generic to multiple renderers. This can
 * aid specifically with libghostty-vt with converting terminal state to
 * a renderable form. */

namespace detail {

/* Wisp: std.unicode.utf8CodepointSequenceLength. False is
 * error.CodepointTooLarge, upstream's only error for this call. */
inline bool utf8CodepointSequenceLength(uint32_t cp, size_t *out) {
    if (cp < 0x80) {
        *out = 1;
        return true;
    }
    if (cp < 0x800) {
        *out = 2;
        return true;
    }
    if (cp < 0x10000) {
        *out = 3;
        return true;
    }
    if (cp < 0x110000) {
        *out = 4;
        return true;
    }
    return false;
}

} /* namespace detail */

/* Contains the state required to render the screen, including optimizing
 * for repeated render calls and only rendering dirty regions.
 *
 * Previously, our renderer would use `clone` to clone the screen within
 * the viewport to perform rendering. This worked well enough that we kept
 * it all the way up through the Ghostty 1.2.x series, but the clone time
 * was repeatedly a bottleneck blocking IO.
 *
 * Rather than a generic clone that tries to clone all screen state per call
 * (within a region), a stateful approach that optimizes for only what a
 * renderer needs to do makes more sense.
 *
 * To use this, initialize the render state to empty, then call `update`
 * on each frame to update the state to the latest terminal state.
 *
 *     var state: RenderState = .empty;
 *     defer state.deinit(alloc);
 *     state.update(alloc, &terminal);
 *
 * ## Two-Phase Updates
 *
 * For callers that synchronize terminal access (e.g. a renderer thread
 * sharing a lock with an IO thread), the update can be split into two
 * phases to minimize the time the terminal must be held exclusively:
 * `beginUpdate` requires terminal access, while `endUpdate` completes
 * any deferred work using only memory owned by the render state.
 *
 *     {
 *         mutex.lock();
 *         defer mutex.unlock();
 *         try state.beginUpdate(alloc, &terminal);
 *     }
 *
 *     // The IO thread is free to modify the terminal while we
 *     // complete the update.
 *     state.endUpdate();
 *
 * The render state must be treated as incomplete between the two calls.
 * `update` is a convenience that performs both phases in one call.
 *
 * ## Memory
 *
 * Note: the render state retains as much memory as possible between updates
 * to prevent future allocations. If a very large frame is rendered once,
 * the render state will retain that much memory until deinit. To avoid
 * waste, it is recommended that the caller `deinit` and start with an
 * empty render state every so often. */
struct RenderState {
    typedef zigstd::ArenaAllocator ArenaAllocator;
    typedef zigstd::Allocator Allocator;
    typedef style::Style Style;
    typedef PageList::Pin Pin;

    /* The color state for the terminal.
     *
     * The background/foreground will be reversed if the terminal reverse
     * color mode is on! You do not need to handle that manually! */
    struct Colors {
        terminal::RGB background;
        terminal::RGB foreground;
        Maybe<terminal::RGB> cursor;
        terminal::Palette palette;
    };

    struct Cursor {
        struct Viewport {
            /* The x/y position of the cursor within the viewport. */
            size::CellCountInt x;
            size::CellCountInt y;

            /* Whether the cursor is part of a wide character and
             * on the tail of it. If so, some renderers may use this
             * to move the cursor back one. */
            bool wide_tail;

            Viewport() : x(0), y(0), wide_tail(false) {}
        };

        /* The x/y position of the cursor within the active area. */
        point::Coordinate active;

        /* The x/y position of the cursor within the viewport. This
         * may be null if the cursor is not visible within the viewport. */
        Maybe<Viewport> viewport;

        /* The cell data for the cursor position. Managed memory is not
         * safe to access from this. */
        page::Cell cell;

        /* The style, always valid even if the cell is default style. */
        Style style;

        /* The visual style of the cursor itself, such as a block or
         * bar. */
        Screen::CursorStyle visual_style;

        /* True if the cursor is detected to be at a password input field. */
        bool password_input;

        /* Cursor visibility state determined by the terminal mode. */
        bool visible;

        /* Cursor blink state determined by the terminal mode. */
        bool blinking;
    };

    struct Highlight {
        /* A special tag that can be used by the caller to differentiate
         * different highlight types. The value is opaque to the RenderState. */
        uint8_t tag;

        /* The x ranges of highlights within this row.
         * Wisp: `[2]size.CellCountInt`. */
        size::CellCountInt range[2];
    };

    struct Cell {
        /* Always set, this is the raw copied cell data from page.Cell.
         * The managed memory (hyperlinks, graphames, etc.) is NOT safe
         * to access from here. It is duplicated into the other fields if
         * it exists. */
        page::Cell raw;

        /* Grapheme data for the cell. This is undefined unless the
         * raw cell's content_tag is `codepoint_grapheme`.
         * Wisp: `[]const u21` is a pointer plus a length. */
        const uint32_t *grapheme;
        size_t grapheme_len;

        /* The style data for the cell. This is undefined unless
         * the style_id is non-default on raw. */
        Style style;
    };

    /* Dirty state.
     * Wisp: `lib.Enum(lib.target, ...)` is a plain enum class. `false` is
     * a C++ keyword, hence the trailing underscore. */
    enum class Dirty : uint8_t {
        /* Not dirty at all. Can skip rendering if prior state was
         * already rendered. */
        false_,

        /* Some rows changed but not all. None of the global state
         * changed such as colors. */
        partial,

        /* Global state changed or dimensions changed. All rows should
         * be redrawn. */
        full,
    };

    /* A run of cells within one row sharing one style, pending
     * denormalization into the per-cell data. This is populated by
     * `beginUpdate` and consumed by `endUpdate`. This exists so that
     * the (potentially large) denormalization of styles into cells
     * can happen outside of any terminal locks. See `beginUpdate`. */
    struct StyleRun {
        /* The viewport row. */
        size::CellCountInt y;

        /* Start (inclusive) and end (exclusive) x coordinates. */
        size::CellCountInt start;
        size::CellCountInt end;

        /* The style for this cell range. */
        Style style;
    };

    /* Wisp: upstream's `?[2]size.CellCountInt` row selection bound. */
    struct SelectionRange {
        size::CellCountInt start;
        size::CellCountInt end;

        SelectionRange() : start(0), end(0) {}
        SelectionRange(size::CellCountInt s, size::CellCountInt e) : start(s), end(e) {}
    };

    /* A row within the viewport. */
    struct Row {
        /* Arena used for any heap allocations for cell contents
         * in this row. Importantly, this is NOT used for the MultiArrayList
         * itself. We do this on purpose so that we can easily clear rows,
         * but retain cached MultiArrayList capacities since grid sizes don't
         * change often. */
        ArenaAllocator::State arena;

        /* The page pin. Its copied values may be compared, but its node must
         * not be dereferenced unless the terminal state is protected from
         * changes since the last `update` call. */
        Pin pin;

        /* The page node generation captured alongside `pin`. This lets
         * consumers validate the pin without dereferencing its node after
         * the terminal lock has been released. */
        uint64_t serial;

        /* Raw row data. */
        page::Row raw;

        /* The cells in this row. Guaranteed to be `cols` length. */
        zigstd::ArrayListUnmanaged<Cell> cells;

        /* A dirty flag that can be used by the renderer to track
         * its own draw state. `update` will mark this true whenever
         * this row is changed, too. */
        bool dirty;

        /* The x range of the selection within this row. */
        Maybe<SelectionRange> selection;

        /* The highlights within this row. */
        zigstd::ArrayListUnmanaged<Highlight> highlights;

        /* The style runs applied to this row's per-cell style data
         * by the last `endUpdate` that touched it. This is what lets
         * `endUpdate` skip the (comparatively large) per-cell style
         * fill when a rebuilt row produced identical runs, which is
         * the common case: text changes far more often than styling.
         *
         * The invariant is that this always describes the current
         * contents of the cell style data for the covered ranges. It
         * is cleared whenever the cell storage is reallocated.
         *
         * This uses the general allocator (NOT the row arena)
         * because it must survive row rebuilds. `endUpdate` cannot
         * allocate, so `beginUpdate` reserves the capacity. */
        zigstd::ArrayListUnmanaged<StyleRun> applied_styles;
    };

    struct SelectionCache {
        Selection selection;
        Pin tl_pin;
        Pin br_pin;
    };

    /* The current screen dimensions. It is possible that these don't match
     * the renderer's current dimensions in grid cells because resizing
     * can happen asynchronously. For example, for Metal, our NSView resizes
     * at a different time than when our internal terminal state resizes.
     * This can lead to a one or two frame mismatch a renderer needs to
     * handle.
     *
     * The viewport is always exactly equal to the active area size so this
     * is also the viewport size. */
    size::CellCountInt rows;
    size::CellCountInt cols;

    /* The color state for the terminal. */
    Colors colors;

    /* Cursor state within the viewport. */
    Cursor cursor;

    /* The rows (y=0 is top) of the viewport. Guaranteed to be `rows` length.
     *
     * This is a MultiArrayList because only the update cares about
     * the allocators. Callers care about all the other properties, and
     * this better optimizes cache locality for read access for those
     * use cases. */
    zigstd::ArrayListUnmanaged<Row> row_data;

    /* The dirty state of the render state. This is set by the update method.
     * The renderer/caller should set this to false when it has handled
     * the dirty state. */
    Dirty dirty;

    /* The screen type that this state represents. This is used primarily
     * to detect changes. */
    ScreenSet::Key screen;

    /* The last viewport pin used to generate this state. This is NOT
     * a tracked pin and is generally NOT safe to read other than the direct
     * values for comparison. */
    Maybe<Pin> viewport_pin; /* = null */

    /* The cached selection so we can avoid expensive selection calculations
     * if possible. */
    Maybe<SelectionCache> selection_cache; /* = null */

    /* The pending style runs requiring an endUpdate call, in the
     * order they were recorded. If multiple begins happen without an
     * endUpdate call, runs accumulate; rows rebuilt more than once
     * may then have superseded (stale) runs in this list, which is
     * harmless: newer runs are appended later so they win, and cells
     * not covered by newer runs have a default style ID in their raw
     * data so their style is undefined by contract anyway. See
     * beginUpdate. */
    zigstd::ArrayListUnmanaged<StyleRun> pending_styles; /* = .empty */

    /* Initial state.
     * Wisp: upstream's `pub const empty: RenderState = .{...}` default-struct
     * pattern is the default constructor, plus a named `empty()` so that call
     * sites read like upstream's `var state: RenderState = .empty;`. */
    RenderState() {
        rows = 0;
        cols = 0;
        colors.background = terminal::RGB(0, 0, 0);
        colors.foreground = terminal::RGB(0xff, 0xff, 0xff);
        colors.cursor = Maybe<terminal::RGB>::none();
        colors.palette = terminal::default_palette();
        cursor.active = point::Coordinate(0, 0);
        cursor.viewport = Maybe<Cursor::Viewport>::none();
        cursor.cell = page::Cell();
        /* Wisp: upstream is `.style = undefined`; it relies on the invariant
         * that beginUpdate always assigns it before any read. Value-initialize
         * instead, which is strictly more defined and identical whenever that
         * invariant holds (see PORTING_CONVENTIONS.md on `undefined`). */
        cursor.style = Style();
        cursor.visual_style = Screen::CursorStyle::block;
        cursor.password_input = false;
        cursor.visible = true;
        cursor.blinking = false;
        /* row_data, pending_styles: `.empty` is their default constructor. */
        dirty = Dirty::false_;
        screen = ScreenSet::Key::primary;
    }

    static RenderState empty() { return RenderState(); }

    void deinit(Allocator alloc) {
        /* Wisp: one loop over the rows rather than three parallel slices. */
        for (size_t i = 0; i < row_data.items_len; i++) {
            Row *r = &row_data.items[i];
            ArenaAllocator arena = r->arena.promote(alloc);
            arena.deinit();
            r->cells.deinit(alloc);
            r->applied_styles.deinit(alloc);
        }
        row_data.deinit(alloc);
        pending_styles.deinit(alloc);
    }

    /* Update the render state to the latest terminal state.
     *
     * This is a convenience function that performs a full update in
     * one call, equivalent to `beginUpdate` immediately followed by
     * `endUpdate`. Callers that hold a lock over the terminal state
     * should prefer calling the two phases directly so that the lock
     * is only held for `beginUpdate`.
     *
     * This will reset the terminal dirty state since it is consumed
     * by this render state update.
     *
     * Wisp: false is OutOfMemory. */
    bool update(Allocator alloc, Terminal *t) {
        if (!beginUpdate(alloc, t)) return false;
        endUpdate();
        return true;
    }

    /* Declared here, defined below RowBuilder. */
    bool beginUpdate(Allocator alloc, Terminal *t);

    /* Complete a prior `beginUpdate` call by performing any deferred
     * work. At the time of writing, this denormalizes the pending
     * style runs into the per-cell style data.
     *
     * This only reads and writes memory owned by the render state, so
     * it is safe to call while the terminal is being modified (no
     * terminal lock is required). */
    void endUpdate() {
        /* Common case: no styled rows were rebuilt. */
        if (pending_styles.items_len == 0) return;

        Row *row_data_items = row_data.items;
        const size_t row_cells_len = row_data.items_len;

        /* Process the pending runs one row segment at a time. All the
         * runs for a row are appended contiguously by a single
         * beginUpdate, so a segment boundary is simply a change in y. */
        const StyleRun *runs = pending_styles.items;
        const size_t runs_len = pending_styles.items_len;
        size_t i = 0;
        while (i < runs_len) {
            const size::CellCountInt y = runs[i].y;
            size_t j = i + 1;
            while (j < runs_len && runs[j].y == y) j += 1;
            const StyleRun *segment = runs + i;
            const size_t segment_len = j - i;
            i = j;

            /* Defensive: the row data may have changed shape if the
             * caller violated ordering (e.g. an error path skipped an
             * endUpdate between updates). Any update that changes
             * dimensions clears the pending list (redraw), so this
             * should never actually trigger, but the cost is trivial. */
            if ((size_t)y >= row_cells_len) continue;

            /* If the segment matches the runs already denormalized
             * into this row's cell data then the per-cell styles are
             * already correct and the (comparatively large) fill can
             * be skipped entirely. This is the common case: rebuilt
             * rows usually keep their styling (only the text
             * changed), and full redraws of an unchanged screen keep
             * both. */
            zigstd::ArrayListUnmanaged<StyleRun> *applied = &row_data_items[y].applied_styles;
            if (runsEql(applied->items, applied->items_len, segment, segment_len)) continue;

            for (size_t k = 0; k < segment_len; k++) {
                const StyleRun &run = segment[k];
                Cell *cells = row_data_items[run.y].cells.items;
                const size_t styles_len = row_data_items[run.y].cells.items_len;
                const size_t end = (size_t)run.end < styles_len ? (size_t)run.end : styles_len;
                const size_t start = (size_t)run.start < end ? (size_t)run.start : end;

                fillStyles(cells + start, end - start, run.style);
            }

            /* Record what we applied so the next rebuild of this row
             * can skip the fill. beginUpdate reserved the capacity
             * (we cannot allocate here); if it doesn't fit (e.g.
             * segments merged across multiple begins without an end)
             * leave the cache empty, which never matches and simply
             * means the next rebuild applies its runs. */
            applied->clearRetainingCapacity();
            if (applied->capacity >= segment_len) {
                applied->appendSliceAssumeCapacity(segment, segment_len);
            }
        }
        pending_styles.clearRetainingCapacity();
    }

    /* Mark all render-state data as consumed by the renderer.
     *
     * This clears both the global dirty state and every per-row dirty flag.
     * Callers that only consume part of a frame should clear the two layers
     * individually instead. */
    void clean() {
        dirty = Dirty::false_;
        for (size_t i = 0; i < row_data.items_len; i++) row_data.items[i].dirty = false;
    }

    /* Fill a slice of styles with one value.
     *
     * Wisp: upstream manually vectorizes this with two overlapping 16-byte
     * vector stores per element, because Zig 0.16's `@memset` with a struct
     * value lowers to a per-element field copy and auto-vectorization is
     * disabled by an LLVM bug. The optimization is not observable; this is
     * the plain loop upstream says it is equivalent to. Note the port's
     * per-cell style lives inside RenderState.Cell rather than in a
     * separate parallel array, so the stride is a Cell, not a Style. */
    static void fillStyles(Cell *dst, size_t len, const Style &value) {
        for (size_t i = 0; i < len; i++) dst[i].style = value;
    }

    /* Returns true if the two style run lists denormalize to
     * identical per-cell style data. This is a semantic comparison
     * (styles are compared field-wise, never by bytes, since padding
     * is undefined). */
    static bool runsEql(const StyleRun *a, size_t a_len, const StyleRun *b, size_t b_len) {
        if (a_len != b_len) return false;
        for (size_t i = 0; i < a_len; i++) {
            if (a[i].start != b[i].start || a[i].end != b[i].end || !a[i].style.eql(b[i].style)) return false;
        }
        return true;
    }

    /* Update the highlights in the render state from the given flattened
     * highlights. Because this uses flattened highlights, it does not require
     * reading from the terminal state so it should be done outside of
     * any critical sections.
     *
     * This will not clear any previous highlights, so the caller must
     * manually clear them if desired.
     *
     * Wisp: false is OutOfMemory. The `[]const highlight.Flattened` slice is
     * a pointer plus a length. */
    bool updateHighlightsFlattened(Allocator alloc, uint8_t tag, const highlight::Flattened *hls, size_t hls_len) {
        /* Fast path, we have no highlights! */
        if (hls_len == 0) return true;

        /* This is, admittedly, horrendous. This is some low hanging fruit
         * to optimize. In my defense, screens are usually small, the number
         * of highlights is usually small, and this only happens on the
         * viewport outside of a locked area. Still, I'd love to see this
         * improved someday. */

        /* We need to track whether any row had a match so we can mark
         * the dirty state. */
        bool any_dirty = false;

        Row *row_data_items = row_data.items;
        const size_t row_len = row_data.items_len;
        for (size_t ri = 0; ri < row_len; ri++) {
            ArenaAllocator::State *row_arena = &row_data_items[ri].arena;
            const Pin row_pin = row_data_items[ri].pin;
            const uint64_t row_serial = row_data_items[ri].serial;
            zigstd::ArrayListUnmanaged<Highlight> *row_highlights = &row_data_items[ri].highlights;
            bool *dirty_flag = &row_data_items[ri].dirty;

            for (size_t hi = 0; hi < hls_len; hi++) {
                const highlight::Flattened &hl = hls[hi];
                /* Wisp: the MultiArrayList field slices are one vector of
                 * structs, so the four parallel arrays are one array. */
                const highlight::Flattened::Chunk *chunks = hl.chunks.data();
                const size_t nodes_len = hl.chunks.size();
                for (size_t i = 0; i < nodes_len; i++) {
                    PageList::Node *node = chunks[i].node;
                    /* If this node generation doesn't match or we're not
                     * within the row range, skip it. Both serials are copied
                     * values, so this never dereferences a node outside the
                     * terminal lock. */
                    if (node != row_pin.node || chunks[i].serial != row_serial || row_pin.y < chunks[i].start ||
                        row_pin.y >= chunks[i].end)
                        continue;

                    /* We're a match! */
                    ArenaAllocator arena = row_arena->promote(alloc);
                    Allocator arena_alloc = arena.allocator();
                    Highlight h;
                    h.tag = tag;
                    h.range[0] = (i == 0 && row_pin.y == chunks[0].start) ? hl.top_x : (size::CellCountInt)0;
                    h.range[1] = (i == nodes_len - 1 && row_pin.y == chunks[nodes_len - 1].end - 1)
                                     ? hl.bot_x
                                     : (size::CellCountInt)(cols - 1);
                    const bool ok = row_highlights->append(arena_alloc, h);
                    /* Wisp: `defer row_arena.* = arena.state;` */
                    *row_arena = arena.state();
                    if (!ok) return false;

                    *dirty_flag = true;
                    any_dirty = true;
                }
            }
        }

        /* Mark our dirty state. */
        if (any_dirty && dirty == Dirty::false_) dirty = Dirty::partial;
        return true;
    }

    typedef zigstd::ArrayListUnmanaged<point::Coordinate> StringMap;

    /* Convert the current render state contents to a UTF-8 encoded
     * string written to the given writer. This will unwrap all the wrapped
     * rows. This is useful for a minimal viewport search.
     *
     * This currently writes empty cell contents as \x00 and writes all
     * blank lines. This is fine for our current usage (link search) but
     * we can adjust this later.
     *
     * NOTE: There is a limitation in that wrapped lines before/after
     * the top/bottom line of the viewport are not included, since
     * the render state cuts them off.
     *
     * Wisp: `*std.Io.Writer` is a `std::string *` (writes cannot fail, so
     * error.WriteFailed is only reachable from the codepoint length check).
     * Upstream's `?struct { alloc, map }` is a nullable map pointer plus the
     * allocator. False is OutOfMemory or WriteFailed. */
    bool string(std::string *writer, StringMap *map, Allocator alloc) const {
        const Row *row_items = row_data.items;
        const size_t row_len = row_data.items_len;

        for (size_t y = 0; y < row_len; y++) {
            const page::Row &row = row_items[y].raw;
            const Cell *cells_slice = row_items[y].cells.items;
            const size_t cells_slice_len = row_items[y].cells.items_len;
            for (size_t x = 0; x < cells_slice_len; x++) {
                const page::Cell &cell = cells_slice[x].raw;
                size_t len = 0;
                if (!detail::utf8CodepointSequenceLength(cell.codepoint(), &len)) return false;
                formatter::detail::writeUtf8(writer, cell.codepoint());
                if (cell.hasGrapheme()) {
                    const uint32_t *graphemes = cells_slice[x].grapheme;
                    const size_t graphemes_len = cells_slice[x].grapheme_len;
                    for (size_t gi = 0; gi < graphemes_len; gi++) {
                        const uint32_t cp = graphemes[gi];
                        size_t cp_len = 0;
                        if (!detail::utf8CodepointSequenceLength(cp, &cp_len)) return false;
                        len += cp_len;
                        formatter::detail::writeUtf8(writer, cp);
                    }
                }

                if (map != nullptr) {
                    /* Wisp: appendNTimes. */
                    const point::Coordinate c((size::CellCountInt)x, (uint32_t)y);
                    for (size_t n = 0; n < len; n++) {
                        if (!map->append(alloc, c)) return false;
                    }
                }
            }

            if (!row.wrap()) {
                writer->append("\n");
                if (map != nullptr) {
                    if (!map->append(alloc, point::Coordinate((size::CellCountInt)cells_slice_len, (uint32_t)y)))
                        return false;
                }
            }
        }
        return true;
    }

    /* A set of coordinates representing cells.
     *
     * Wisp: `std.AutoArrayHashMapUnmanaged(point.Coordinate, void)` is an
     * insertion-ordered list of the keys with a linear-scan lookup. The
     * viewport is at most rows*cols cells, and callers only count and
     * test membership. */
    struct CellSet {
        zigstd::ArrayListUnmanaged<point::Coordinate> keys;

        void deinit(Allocator alloc) { keys.deinit(alloc); }

        size_t count() const { return keys.items_len; }

        bool contains(const point::Coordinate &c) const {
            for (size_t i = 0; i < keys.items_len; i++) {
                if (keys.items[i].eql(c)) return true;
            }
            return false;
        }

        /* Wisp: false is OutOfMemory. */
        bool put(Allocator alloc, const point::Coordinate &c) {
            if (contains(c)) return true;
            return keys.append(alloc, c);
        }
    };

    /* Returns a map of the cells that match to an OSC8 hyperlink over the
     * given point in the render state.
     *
     * IMPORTANT: The terminal must not have updated since the last call to
     * `update`. If there is any chance the terminal has updated, the caller
     * must first call `update` again to refresh the render state.
     *
     * For example, you may want to hold a lock for the duration of the
     * update and hyperlink lookup to ensure no updates happen in between.
     *
     * Wisp: the result is an out-param; false is OutOfMemory. */
    bool linkCells(Allocator alloc, const point::Coordinate &viewport_point, CellSet *out) const {
        CellSet result;

        const Row *row_items = row_data.items;
        const size_t row_pins_len = row_data.items_len;

        /* Our viewport point is sent in by the caller and can't be trusted.
         * If it is outside the valid area then just return empty because
         * we can't possibly have a link there. */
        if (viewport_point.x >= cols || viewport_point.y >= row_pins_len) {
            *out = result;
            return true;
        }

        /* Grab our link ID */
        const Pin link_pin = row_items[viewport_point.y].pin;
        page::Page *link_page = link_pin.node->page();
        const hyperlink::PageEntry *link = nullptr;
        {
            const page::Page::RowAndCell rac = link_page->getRowAndCell(viewport_point.x, link_pin.y);

            /* The likely scenario is that our mouse isn't even over a link. */
            if (!rac.cell->hyperlink()) {
                *out = result;
                return true;
            }

            hyperlink::Id link_id = 0;
            if (!link_page->lookupHyperlink(rac.cell, &link_id)) {
                *out = result;
                return true;
            }
            link = link_page->hyperlink_set.get((const void *)link_page->memory, link_id);
        }

        for (size_t y = 0; y < row_pins_len; y++) {
            const Pin pin = row_items[y].pin;
            const Cell *cells = row_items[y].cells.items;
            const size_t cells_len = row_items[y].cells.items_len;
            for (size_t x = 0; x < cells_len; x++) {
                if (!cells[x].raw.hyperlink()) continue;

                page::Page *other_page = pin.node->page();
                const hyperlink::PageEntry *other = nullptr;
                {
                    const page::Page::RowAndCell rac = other_page->getRowAndCell(x, pin.y);
                    hyperlink::Id link_id = 0;
                    if (!other_page->lookupHyperlink(rac.cell, &link_id)) continue;
                    other = other_page->hyperlink_set.get((const void *)other_page->memory, link_id);
                }

                if (link->eql((const void *)link_page->memory, *other, (const void *)other_page->memory)) {
                    if (!result.put(alloc, point::Coordinate((size::CellCountInt)x, (uint32_t)y))) {
                        /* Wisp: `errdefer result.deinit(alloc)` */
                        result.deinit(alloc);
                        return false;
                    }
                }
            }
        }

        *out = result;
        return true;
    }
};

/* The number of rows/cells we scan as a single group when looking for
 * dirty rows or special cells. Rows and cells are small packed structs
 * so a group is scanned with a handful of vector operations. */
static const size_t scan_group_len = 8;

/* Group scan helper for the row dirty flag. A row that matches has
 * its dirty flag unset.
 * Wisp: page::Mask's field mask is a run-time value, so these are
 * functions returning the one shared mask value. */
typedef page::Mask<page::Row, scan_group_len> RowDirtyMaskT;
inline RowDirtyMaskT RowDirtyMask() { return RowDirtyMaskT(page::fields::row_dirty()); }

/* Group scan helper for the cell fields that require managed memory
 * handling. A cell that matches is a plain (possibly zero) codepoint
 * with a default style, requiring no work beyond the raw copy. See
 * RowBuilder.row. */
typedef page::Mask<page::Cell, scan_group_len> CellSpecialMaskT;
inline CellSpecialMaskT CellSpecialMask() {
    return CellSpecialMaskT(page::fields::cell_content_tag() | page::fields::cell_style_id());
}

/* Internal helper for RenderState.update that rebuilds a single row of
 * the render state from the current page contents.
 *
 * Wisp: upstream holds one slice per MultiArrayList field; this holds the
 * one array of Row structs instead (see the file header). */
struct RowBuilder {
    zigstd::Allocator alloc;
    size_t cols;
    RenderState::Row *rows;
    zigstd::ArrayListUnmanaged<RenderState::StyleRun> *pending_styles;

    /* Wisp: `defer b.arenas[vy] = arena.state;` */
    struct ArenaGuard {
        zigstd::ArenaAllocator::State *dst;
        zigstd::ArenaAllocator *arena;
        ArenaGuard(zigstd::ArenaAllocator::State *d, zigstd::ArenaAllocator *a) : dst(d), arena(a) {}
        ~ArenaGuard() { *dst = arena->state(); }
    };

    /* Wisp: false is OutOfMemory. */
    bool row(page::Page *p, const page::Row *page_row, size_t vy) const {
        RenderState::Row *rr = &rows[vy];

        /* Promote our arena. State is copied by value so we need to
         * restore it on all exit paths so we don't leak memory. */
        zigstd::ArenaAllocator arena = rr->arena.promote(alloc);
        ArenaGuard arena_guard(&rr->arena, &arena);

        /* Reset our per-row state if we're rebuilding this row. A
         * non-zero cell length means the row was populated by a prior
         * update. */
        if (rr->cells.items_len > 0) {
            (void)arena.reset(zigstd::ArenaAllocator::ResetMode::retain_capacity);
            rr->selection = Maybe<RenderState::SelectionRange>::none();
            rr->highlights = zigstd::ArrayListUnmanaged<RenderState::Highlight>();
        }
        rr->dirty = true;

        /* Get all our cells in the page. */
        const page::Cell *page_cells = page_row->cells().ptr(p->memory);

        /* Copy our raw row data */
        rr->raw = *page_row;

        /* Note: our cells MultiArrayList uses our general allocator.
         * We do this on purpose because as rows become dirty, we do
         * not want to reallocate space for cells (which are large). This
         * was a source of huge slowdown.
         *
         * Our per-row arena is only used for temporary allocations
         * pertaining to cells directly (e.g. graphemes, hyperlinks). */
        zigstd::ArrayListUnmanaged<RenderState::Cell> *cells = &rr->cells;
        if (cells->items_len != cols) {
            /* The cell storage (including the per-cell style data) is
             * being reallocated, so the applied style cache no longer
             * describes it. Clear it before the resize so an error
             * can't leave it stale. */
            rr->applied_styles.clearRetainingCapacity();
            if (!cells->resize(alloc, cols)) return false;
        }

        /* We always copy our raw cell data. In the case we have no
         * managed memory, we can skip setting any other fields.
         *
         * This is an important optimization. For plain-text screens
         * this ends up being something around 300% faster based on
         * the `screen-clone` benchmark.
         *
         * Wisp: the raw cells are a field of RenderState.Cell rather than
         * their own parallel array, so this is a strided per-cell copy
         * instead of fastmem.copy of a contiguous slice. */
        RenderState::Cell *cells_slice = cells->items;
        for (size_t i = 0; i < cols; i++) cells_slice[i].raw = page_cells[i];
        if (!page_row->managedMemory()) return true;

        zigstd::Allocator arena_alloc = arena.allocator();
        const size_t n = cols;
        const size_t runs_start = pending_styles->items_len;
        size_t x = 0;
        const CellSpecialMaskT cell_mask = CellSpecialMask();
        while (x < n) {
            /* Skip runs of plain cells a group at a time. Cells that
             * need managed handling are often rare even within rows that
             * have managed memory (e.g. a row is "styled" if a single
             * cell has a style) so groups are skipped with a few vector
             * operations. */
            while (n - x >= CellSpecialMaskT::group_len) {
                if (!cell_mask.match(page_cells, x)) break;
                x += CellSpecialMaskT::group_len;
            }

            /* Scalar scan to the next special cell. */
            bool done = false;
            while (true) {
                if (x >= n) {
                    done = true;
                    break;
                }
                if (!cell_mask.matchScalar(page_cells[x])) break;
                x += 1;
            }
            if (done) break; /* Wisp: `break :scan` */

            const page::Cell *page_cell = &page_cells[x];

            switch (page_cell->content_tag()) {
            /* Single-codepoint styled cells are by far the most
             * common special cells, and they usually come in long
             * runs sharing one style ID (e.g. a fully styled row
             * usually uses a single style). Find the run and record
             * it: this does one style lookup per run and defers the
             * (large) per-cell fill to endUpdate, outside of any
             * terminal locks. */
            case page::Cell::ContentTag::codepoint: {
                const style::Id sid = page_cell->style_id();
                assert(sid > 0); /* special + codepoint implies styled */
                const style::Style style_val = *p->styles.get((const void *)p->memory, sid);

                /* A cell continues the run if its masked special
                 * bits are exactly the style ID of the run (in
                 * particular the content tag must be a plain
                 * codepoint). We can check groups of cells at a
                 * time this way. */
                const CellSpecialMaskT::Backing pattern = cell_mask.pattern(*page_cell);
                const size_t start = x;
                x += 1;
                while (n - x >= CellSpecialMaskT::group_len) {
                    if (!cell_mask.eql(page_cells, x, pattern)) break;
                    x += CellSpecialMaskT::group_len;
                }
                for (; x < n; x += 1) {
                    if (!cell_mask.eqlScalar(page_cells[x], pattern)) break;
                }

                RenderState::StyleRun run;
                run.y = (size::CellCountInt)vy;
                run.start = (size::CellCountInt)start;
                run.end = (size::CellCountInt)x;
                run.style = style_val;
                if (!pending_styles->append(alloc, run)) return false;
                break;
            }

            /* If we have a multi-codepoint grapheme, look it up and
             * set our content type. Note grapheme cells may also
             * be styled. The style must be recorded as a run (rather
             * than written directly) so that it is ordered correctly
             * relative to possibly-stale runs from prior updates. */
            case page::Cell::ContentTag::codepoint_grapheme: {
                if (page_cell->style_id() > 0) {
                    RenderState::StyleRun run;
                    run.y = (size::CellCountInt)vy;
                    run.start = (size::CellCountInt)x;
                    run.end = (size::CellCountInt)(x + 1);
                    run.style = *p->styles.get((const void *)p->memory, page_cell->style_id());
                    if (!pending_styles->append(alloc, run)) return false;
                }
                size_t glen = 0;
                const uint32_t *gp = p->lookupGrapheme(page_cell, &glen);
                if (gp == nullptr) glen = 0;
                uint32_t *dup = arena_alloc.allocT<uint32_t>(glen);
                if (dup == nullptr) return false;
                if (glen > 0) fastmem::copy(dup, gp, glen);
                cells_slice[x].grapheme = dup;
                cells_slice[x].grapheme_len = glen;
                x += 1;
                break;
            }

            /* Background-color-only cells. The style is derived
             * entirely from the cell contents. Consecutive cleared
             * cells with the same background are bit-identical, so
             * we run-detect on full equality (e.g. a line cleared
             * with a background color pending is one run). */
            case page::Cell::ContentTag::bg_color_rgb:
            case page::Cell::ContentTag::bg_color_palette: {
                /* Wisp: upstream's `else => unreachable` arm cannot be
                 * reached because this switch only covers the two bg tags;
                 * the value is initialized from the tag instead of left
                 * indeterminate (see PORTING_CONVENTIONS.md). */
                style::Style style_val;
                if (page_cell->content_tag() == page::Cell::ContentTag::bg_color_rgb) {
                    const page::Cell::RGB c = page_cell->contentColorRgb();
                    style_val.bg_color = style::Style::Color::makeRgb(terminal::RGB(c.r, c.g, c.b));
                } else {
                    style_val.bg_color = style::Style::Color::makePalette(page_cell->contentColorPalette());
                }

                const CellSpecialMaskT::Backing first_bits = CellSpecialMaskT::bits(*page_cell);
                const size_t start = x;
                x += 1;
                while (n - x >= CellSpecialMaskT::group_len) {
                    if (!cell_mask.eqlExact(page_cells, x, first_bits)) break;
                    x += CellSpecialMaskT::group_len;
                }
                for (; x < n; x += 1) {
                    if (CellSpecialMaskT::bits(page_cells[x]) != first_bits) break;
                }

                RenderState::StyleRun run;
                run.y = (size::CellCountInt)vy;
                run.start = (size::CellCountInt)start;
                run.end = (size::CellCountInt)x;
                run.style = style_val;
                if (!pending_styles->append(alloc, run)) return false;
                break;
            }
            }
        }

        /* Reserve the applied style cache capacity for the runs we
         * appended so that endUpdate (which cannot allocate) is able
         * to record what it applies. See Row.applied_styles. */
        const size_t runs_added = pending_styles->items_len - runs_start;
        if (runs_added > 0) {
            if (!rr->applied_styles.ensureTotalCapacity(alloc, runs_added)) return false;
        }
        return true;
    }
};

/* Begin an update of the render state to the latest terminal
 * state. Every begin must be completed with an `endUpdate` call
 * before the render state is read.
 *
 * This two-phase structure exists for callers that lock the
 * terminal state: only this function requires terminal access, so
 * a caller can hold its lock for this call only and then call
 * `endUpdate` after releasing it. `endUpdate` exclusively reads
 * and writes memory owned by the render state.
 *
 * Work that doesn't require terminal access may be deferred to
 * `endUpdate` to keep this call (and therefore lock hold time) as
 * short as possible. At the time of writing, the deferred work is
 * the per-cell style denormalization, so between this call and
 * `endUpdate` the per-cell `style` data of any updated rows is
 * stale and must not be read. More work may be deferred in the
 * future; callers should treat the render state as incomplete
 * until `endUpdate` is called.
 *
 * This will reset the terminal dirty state since it is consumed
 * by this render state update.
 *
 * Wisp: false is OutOfMemory. */
inline bool RenderState::beginUpdate(zigstd::Allocator alloc, Terminal *t) {
    Screen *s = t->screens.active;
    const Pin viewport_pin_v = s->pages.getTopLeft(point::Tag::viewport);
    bool redraw = false;
    do {
        /* If our screen key changed, we need to do a full rebuild
         * because our render state is viewport-specific. */
        if (t->screens.active_key != screen) {
            redraw = true;
            break;
        }

        /* If our terminal is dirty at all, we do a full rebuild. These
         * dirty values are full-terminal dirty values.
         * Wisp: `@bitCast` of the packed struct to its backing integer and
         * `> 0` is "any flag set". */
        {
            const Terminal::Dirty &d = t->flags.dirty;
            if (d.palette || d.reverse_colors || d.clear || d.preedit) {
                redraw = true;
                break;
            }
        }

        /* If our screen is dirty at all, we do a full rebuild. This is
         * a full screen dirty tracker. */
        {
            const Screen::Dirty &d = t->screens.active->dirty;
            if (d.selection || d.hyperlink_hover) {
                redraw = true;
                break;
            }
        }

        /* If our dimensions changed, we do a full rebuild. */
        if (rows != s->pages.rows || cols != s->pages.cols) {
            redraw = true;
            break;
        }

        /* If our viewport pin changed, we do a full rebuild. */
        if (viewport_pin.has) {
            if (!viewport_pin.value.eql(viewport_pin_v)) {
                redraw = true;
                break;
            }
        }
    } while (0);

    /* Always set our cheap fields, its more expensive to compare */
    rows = s->pages.rows;
    cols = s->pages.cols;
    viewport_pin = Maybe<Pin>(viewport_pin_v);
    cursor.active = point::Coordinate(s->cursor.x, s->cursor.y);
    cursor.cell = *s->cursor.page_cell;
    cursor.style = s->cursor.style;
    cursor.visual_style = s->cursor.cursor_style;
    cursor.password_input = t->flags.password_input;
    cursor.visible = t->modes.get(terminal::modes::Mode::cursor_visible);
    cursor.blinking = t->modes.get(terminal::modes::Mode::cursor_blinking);

    /* Always reset the cursor viewport position. In the future we can
     * probably cache this by comparing the cursor pin and viewport pin
     * but may not be worth it. */
    cursor.viewport = Maybe<Cursor::Viewport>::none();

    /* Colors. */
    {
        terminal::RGB c;
        colors.cursor = t->colors.cursor.get(&c) ? Maybe<terminal::RGB>(c) : Maybe<terminal::RGB>::none();
    }

    /* The palette is a relatively large copy (768 bytes at the time
     * of writing) so we only copy it when it could have changed. All
     * palette modifications set a terminal-level dirty flag (see
     * Terminal.Dirty.palette), and any terminal-level dirty flag
     * forces a redraw, so checking redraw is sufficient. */
    if (redraw) colors.palette = t->colors.palette.current;

    do { /* Wisp: `bg_fg:` block. */
        /* Background/foreground can be unset initially which would
         * depend on "default" background/foreground. The expected use
         * case of Terminal is that the caller set their own configured
         * defaults on load so this doesn't happen. */
        terminal::RGB bg;
        if (!t->colors.background.get(&bg)) break;
        terminal::RGB fg;
        if (!t->colors.foreground.get(&fg)) break;
        if (t->modes.get(terminal::modes::Mode::reverse_colors)) {
            colors.background = fg;
            colors.foreground = bg;
        } else {
            colors.background = bg;
            colors.foreground = fg;
        }
    } while (0);

    /* Ensure our row length is exactly our height, freeing or allocating
     * data as necessary. In most cases we'll have a perfectly matching
     * size. */
    if (row_data.items_len != (size_t)rows) {
        if (row_data.items_len < (size_t)rows) {
            /* Resize our rows to the desired length, marking any added
             * values undefined. */
            const size_t old_len = row_data.items_len;
            /* Wisp: Zig evaluates `old_len..self.rows` once; `rows` is not
             * mutated below but the bound is snapshotted regardless (see
             * PORTING_CONVENTIONS.md). */
            const size_t new_len = (size_t)rows;
            if (!row_data.resize(alloc, new_len)) return false;

            /* Initialize all our values. Its faster to use slice() + set()
             * because appendAssumeCapacity does this multiple times. */
            Row *row_items = row_data.items;
            for (size_t y = old_len; y < new_len; y++) {
                /* Wisp: upstream leaves `pin`, `serial` and `raw` undefined
                 * here; they are value-initialized instead (see
                 * PORTING_CONVENTIONS.md on `undefined`). */
                Row r;
                r.arena = ArenaAllocator::State();
                r.pin = Pin();
                r.serial = 0;
                r.raw = page::Row();
                r.cells = zigstd::ArrayListUnmanaged<Cell>();
                r.dirty = true;
                r.selection = Maybe<SelectionRange>::none();
                r.highlights = zigstd::ArrayListUnmanaged<Highlight>();
                r.applied_styles = zigstd::ArrayListUnmanaged<StyleRun>();
                row_items[y] = r;
            }
        } else {
            Row *row_items = row_data.items;
            const size_t old_len = row_data.items_len;
            for (size_t y = (size_t)rows; y < old_len; y++) {
                ArenaAllocator arena = row_items[y].arena.promote(alloc);
                arena.deinit();
                row_items[y].cells.deinit(alloc);
                row_items[y].applied_styles.deinit(alloc);
            }
            row_data.shrinkRetainingCapacity((size_t)rows);
        }
    }

    /* Break down our row data.
     * Wisp: one array of Row structs instead of nine field slices. */
    Row *rd = row_data.items;

    /* If we're redrawing then every row will be rebuilt, superseding
     * any pending style runs from prior updates. Clearing also
     * guarantees pending runs always match the current dimensions
     * (dimension changes force a redraw). */
    if (redraw) pending_styles.clearRetainingCapacity();

    /* Go through and setup our rows. We iterate page chunks rather
     * than individual rows so that per-page work (dirty flags, cursor
     * detection, memory pointers) is hoisted out of the row loop. This
     * makes the common case of a clean (or mostly clean) frame very
     * cheap: a contiguous scan of row dirty flags. */
    RowBuilder builder;
    builder.alloc = alloc;
    builder.cols = cols;
    builder.rows = rd;
    builder.pending_styles = &pending_styles;

    size_t y = 0;
    bool any_dirty = false;
    PageList::PageIterator page_it = viewport_pin_v.pageIterator(PageList::Direction::right_down, Maybe<Pin>());
    const RowDirtyMaskT row_dirty_mask = RowDirtyMask();
    while (y < (size_t)rows) {
        PageList::Chunk chunk;
        if (!page_it.next(&chunk)) break;
        PageList::Node *node = chunk.node;
        const uint64_t node_serial = node->serial;
        page::Page *p = node->page();

        /* The number of rows we consume from this chunk. The chunk
         * may extend beyond the viewport (the viewport is always
         * exactly `rows` tall) so we clamp. */
        const size_t chunk_len = (size_t)(chunk.end - chunk.start);
        const size_t remaining = (size_t)rows - y;
        const size_t take = chunk_len < remaining ? chunk_len : remaining;

        /* Find our cursor if we haven't found it yet. We do this even
         * if rows are not dirty because the cursor is unrelated. We
         * can check the chunk bounds once rather than every row. */
        if (!cursor.viewport.has && node == s->cursor.page_pin->node) do {
            const size::CellCountInt cy = s->cursor.page_pin->y;
            if (cy < chunk.start || (size_t)cy >= (size_t)chunk.start + take) break; /* `break :cursor` */
            Cursor::Viewport vp;
            vp.y = (size::CellCountInt)(y + (size_t)(cy - chunk.start));
            vp.x = s->cursor.x;

            /* Future: we should use our own state here to look this
             * up rather than calling this. */
            vp.wide_tail = s->cursor.x > 0 ? s->cursorCellLeft(1)->wide() == page::Cell::Wide::wide : false;
            cursor.viewport = Maybe<Cursor::Viewport>(vp);
        } while (0);

        /* The page-level dirty flag applies to every row in the chunk.
         * We consume (clear) it now; each node appears at most once in
         * this iteration and we're the only consumer of dirty state. */
        const bool page_dirty = p->dirty;
        if (page_dirty) p->dirty = false;

        /* Get our contiguous rows for this chunk. */
        page::Row *page_rows = p->rows.ptr(p->memory) + chunk.start;
        assert(p->size.cols == cols);

        /* Store our pins and their node generations. We have to store
         * these even for rows that aren't dirty because dirty is only a
         * renderer optimization; it doesn't apply to memory movement.
         * This lets us remap any cell pins back to an exact entry in our
         * RenderState and validate them later without dereferencing a
         * potentially stale node.
         *
         * We can skip the writes when the pins and serials are unchanged:
         * if we're not redrawing, every value was stored by a prior update
         * (row count changes force a redraw). Within a single update a
         * node appears at most once and its stored pins have consecutive
         * y values, so if the first and last entries of this chunk's range
         * already match then every entry in between matches too. */
        if (redraw || rd[y].pin.node != node || rd[y].pin.y != chunk.start || rd[y].serial != node_serial ||
            rd[y + take - 1].pin.node != node ||
            (size_t)rd[y + take - 1].pin.y != (size_t)chunk.start + take - 1 ||
            rd[y + take - 1].serial != node_serial) {
            for (size_t i = 0; i < take; i++) {
                rd[y + i].pin = Pin(node, (size::CellCountInt)((size_t)chunk.start + i));
                rd[y + i].serial = node_serial;
            }
        }

        if (!redraw && !page_dirty) {
            /* Only dirty rows (usually none) need a rebuild. Scan the
             * dirty flags a group at a time; the dirty bit is directly
             * testable on the packed row representation. */
            size_t i = 0;
            while (take - i >= RowDirtyMaskT::group_len) {
                if (row_dirty_mask.match(page_rows, i)) {
                    i += RowDirtyMaskT::group_len;
                    continue;
                }

                for (size_t j = i; j < i + RowDirtyMaskT::group_len; j++) {
                    page::Row *page_row = &page_rows[j];
                    if (!page_row->dirty()) continue;
                    page_row->setDirty(false);
                    any_dirty = true;
                    if (!builder.row(p, page_row, y + j)) return false;
                }
                i += RowDirtyMaskT::group_len;
            }
            for (; i < take; i += 1) {
                page::Row *page_row = &page_rows[i];
                if (!page_row->dirty()) continue;
                page_row->setDirty(false);
                any_dirty = true;
                if (!builder.row(p, page_row, y + i)) return false;
            }
        } else {
            /* Rebuild every row in the chunk. */
            any_dirty = true;
            for (size_t i = 0; i < take; i++) {
                page::Row *page_row = &page_rows[i];
                page_row->setDirty(false);
                if (!builder.row(p, page_row, y + i)) return false;
            }
        }

        y += take;
    }
    assert(y == (size_t)rows);

    /* If our screen has a selection, then mark the rows with the
     * selection. We do this outside of the loop above because its unlikely
     * a selection exists and because the way our selections are structured
     * today is very inefficient.
     *
     * NOTE: To improve the performance of the block below, we'll need
     * to rethink how we model selections in general.
     *
     * There are performance improvements that can be made here, though.
     * For example, `containedRow` recalculates a bunch of information
     * we can cache. */
    if (s->selection.has) do { /* Wisp: `selection:` block. */
        Selection *sel = &s->selection.value;

        /* Populate our selection cache to avoid some expensive
         * recalculation. */
        const SelectionCache *cache = nullptr;
        if (selection_cache.has) do { /* Wisp: `cache_check:` block. */
            SelectionCache *c = &selection_cache.value;

            /* If we're redrawing, we recalculate the cache just to
             * be safe. */
            if (redraw) break;

            /* If our selection isn't equal, we aren't cached! */
            if (!c->selection.eql(*sel)) break;

            /* If we have no dirty rows, we can not recalculate. */
            if (!any_dirty) goto selection_done; /* Wisp: `break :selection` */

            /* We have dirty rows, we can utilize the cache. */
            cache = c;
        } while (0);

        if (cache == nullptr) {
            /* Create a new cache */
            const Pin tl_pin = sel->topLeft(s);
            const Pin br_pin = sel->bottomRight(s);
            SelectionCache nc;
            nc.selection = Selection::init(tl_pin, br_pin, sel->rectangle);
            nc.tl_pin = tl_pin;
            nc.br_pin = br_pin;
            selection_cache = Maybe<SelectionCache>(nc);
            cache = &selection_cache.value;
        }

        {
            /* Grab the inefficient data we need from the selection. At
             * least we can cache it. */
            const point::Coordinate tl = s->pages.pointFromPin(point::Tag::screen, cache->tl_pin).value.c;
            const point::Coordinate br = s->pages.pointFromPin(point::Tag::screen, cache->br_pin).value.c;

            /* We need to determine if our selection is within the viewport.
             * The viewport is generally very small so the efficient way to
             * do this is to traverse the viewport pages and check for the
             * matching selection pages. */
            const size_t row_len = row_data.items_len;
            for (size_t ri = 0; ri < row_len; ri++) {
                const Pin pin = rd[ri].pin;
                const point::Coordinate p = s->pages.pointFromPin(point::Tag::screen, pin).value.c;
                const Maybe<Selection> row_sel =
                    sel->containedRowCached(s, cache->tl_pin, cache->br_pin, pin, tl, br, p);
                if (!row_sel.has) continue;
                const Pin start = row_sel.value.start();
                const Pin end = row_sel.value.end();
                assert(start.node == end.node);
                assert(start.x <= end.x);
                assert(start.y == end.y);
                rd[ri].selection = Maybe<SelectionRange>(SelectionRange(start.x, end.x));
            }
        }
    } while (0);
selection_done:

    /* Handle dirty state. */
    if (redraw) {
        /* Fully redraw resets some other state. */
        screen = t->screens.active_key;
        dirty = Dirty::full;

        /* Note: we don't clear any row_data here because our rebuild
         * above did this. */
    } else if (any_dirty && dirty == Dirty::false_) {
        dirty = Dirty::partial;
    }

    /* Clear our dirty flags */
    t->flags.dirty = Terminal::Dirty();
    s->dirty = Screen::Dirty();
    return true;
}

} /* namespace render */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_RENDER_HPP */
