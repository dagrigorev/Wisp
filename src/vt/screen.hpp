/* Transliterated from Ghostty src/terminal/Screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp notes:
 *   - Kitty graphics image storage is not ported yet; this corresponds to
 *     upstream's `build_options.kitty_graphics == false` configuration for
 *     Screen (kitty_images is `struct {}`). The kitty placeholder handling
 *     in page/pagelist code is still enabled.
 *   - `std.Io` is not carried over.
 *   - Allocator.Error!T is bool (false = OutOfMemory); IncreaseCapacityError
 *     is PageList::IncreaseCapacityError.
 *   - testWriteString's width lookup (`unicode.table.get(c).width`) uses
 *     Wisp's codepoint width table (core/unicode.c).
 */

#pragma once
#ifndef WISP_VT_SCREEN_HPP
#define WISP_VT_SCREEN_HPP

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../terminal/ansi.hpp"
#include "../terminal/charsets.hpp"
#include "../terminal/kitty/key.hpp"
#include "../terminal/osc.hpp"
#include "../terminal/sgr.hpp"
#include "fastmem.hpp"
#include "unicode/grapheme.hpp"
#include "page_list.hpp"
#include "selection.hpp"
#include "tripwire.hpp"


namespace wisp {
namespace vt {

namespace formatter {
struct PinMapMap;
}

struct Screen {
    typedef PageList::Pin Pin;
    typedef page::Page Page;
    typedef page::Row Row;
    typedef page::Cell Cell;

    /* cursor.zig: The visual style of the cursor. */
    enum class CursorStyle {
        /* DECSCUSR 5, 6 */
        bar,
        /* DECSCUSR 1, 2 */
        block,
        /* DECSCUSR 3, 4 */
        underline,
        /* Hollow block cursor. This is a block cursor with the center empty.
         * Reported as DECSCUSR 1 or 2 (block). */
        block_hollow,
    };

    /* See Terminal.Dirty. This behaves the same way. */
    struct Dirty {
        /* Set when the selection is set or unset, regardless of if the
         * selection is changed or not. */
        bool selection; /* = false */

        /* When an OSC8 hyperlink is hovered, we set the full screen as dirty
         * because links can span multiple lines. */
        bool hyperlink_hover; /* = false */

        Dirty() : selection(false), hyperlink_hover(false) {}
    };

    struct SemanticPrompt {
        /* This is flipped to true when any sort of semantic content is
         * seen. In particular, this is set to true only when a `prompt` type
         * is ever set on our cursor.
         *
         * This is used to optimize away semantic content operations if we know
         * we've never seen them. */
        bool seen;

        /* This is set on any `cl` or `click_events` option set on the
         * most recent OSC 133 commands to specify how click handling in a
         * prompt is handling. */
        struct SemanticClick {
            enum class Kind { none, click_events, cl } tag;
            terminal::osc::semantic_prompt::ClickEvents click_events;
            terminal::osc::semantic_prompt::Click cl;
        } click;

        static SemanticPrompt disabled() {
            SemanticPrompt p;
            p.seen = false;
            p.click.tag = SemanticClick::Kind::none;
            p.click.click_events = terminal::osc::semantic_prompt::ClickEvents::absolute;
            p.click.cl = (terminal::osc::semantic_prompt::Click)0;
            return p;
        }
    };

    /* The cursor position and style. */
    struct Cursor {
        /* The x/y position within the active area. */
        size::CellCountInt x; /* = 0 */
        size::CellCountInt y; /* = 0 */

        /* The visual style of the cursor. This defaults to block because
         * it has to default to something, but users of this struct are
         * encouraged to set their own default. */
        CursorStyle cursor_style; /* = .block */

        /* The "last column flag (LCF)" as its called. If this is set then the
         * next character print will force a soft-wrap. */
        bool pending_wrap; /* = false */

        /* The protected mode state of the cursor. If this is true then
         * all new characters printed will have the protected state set. */
        bool protected_; /* = false */

        /* The currently active style. This is the concrete style value
         * that should be kept up to date. The style ID to use for cell writing
         * is below. */
        style::Style style;

        /* The currently active style ID. The style is page-specific so when
         * we change pages we need to ensure that we update that page with
         * our style when used. */
        style::Id style_id; /* = style.default_id */

        /* The hyperlink ID that is currently active for the cursor. A value
         * of zero means no hyperlink is active. (Implements OSC8, saying that
         * so code search can find it.). */
        hyperlink::Id hyperlink_id; /* = 0 */

        /* This is the implicit ID to use for hyperlinks that don't specify
         * an ID. We do an overflowing add to this so repeats can technically
         * happen with carefully crafted inputs but for real workloads its
         * highly unlikely -- and the fix is for the TUI program to use explicit
         * IDs. */
        size::OffsetInt hyperlink_implicit_id; /* = 0 */

        /* Heap-allocated hyperlink state so that we can recreate it when
         * the cursor page pin changes. We can't get it from the old screen
         * state because the page may be cleared. This is heap allocated
         * because its most likely null. */
        hyperlink::Hyperlink *hyperlink; /* = null */

        /* The current semantic content type for the cursor that will be
         * applied to any newly written cells. */
        Cell::SemanticContent semantic_content; /* = .output */
        bool semantic_content_clear_eol;         /* = false */

        /* The pointers into the page list where the cursor is currently
         * located. This makes it faster to move the cursor. */
        Pin *page_pin;
        Row *page_row;
        Cell *page_cell;

        Cursor()
            : x(0), y(0), cursor_style(CursorStyle::block), pending_wrap(false), protected_(false), style(),
              style_id(style::default_id), hyperlink_id(0), hyperlink_implicit_id(0), hyperlink(nullptr),
              semantic_content(Cell::SemanticContent::output), semantic_content_clear_eol(false),
              page_pin(nullptr), page_row(nullptr), page_cell(nullptr) {}

        void deinit(zigstd::Allocator alloc) {
            if (hyperlink) {
                hyperlink->deinit(alloc);
                alloc.destroy(hyperlink);
            }
        }
    };

    /* State required for all charset operations. */
    struct CharsetState {
        typedef terminal::charsets::Slots Slots;
        typedef terminal::charsets::Charset Charset;

        /* An array to map a charset slot to a lookup table.
         *
         * We use this bespoke struct instead of `std.EnumArray` because
         * accessing these slots is very performance critical since it's
         * done for every single print. This benchmarks faster. */
        struct CharsetArray {
            Charset g0; /* = .utf8 */
            Charset g1;
            Charset g2;
            Charset g3;

            CharsetArray() : g0(Charset::utf8), g1(Charset::utf8), g2(Charset::utf8), g3(Charset::utf8) {}

            Charset get(Slots slot) const {
                switch (slot) {
                case Slots::G0: return g0;
                case Slots::G1: return g1;
                case Slots::G2: return g2;
                default: return g3;
                }
            }

            void set(Slots slot, Charset charset) {
                switch (slot) {
                case Slots::G0: g0 = charset; break;
                case Slots::G1: g1 = charset; break;
                case Slots::G2: g2 = charset; break;
                case Slots::G3: g3 = charset; break;
                }
            }
        };

        /* The list of graphical charsets by slot */
        CharsetArray charsets;

        /* GL is the slot to use when using a 7-bit printable char (up to 127)
         * GR used for 8-bit printable chars. */
        Slots gl; /* = .G0 */
        Slots gr; /* = .G2 */

        /* Single shift where a slot is used for exactly one char. */
        Maybe<Slots> single_shift; /* = null */

        CharsetState() : charsets(), gl(Slots::G0), gr(Slots::G2), single_shift() {}
    };

    /* Saved cursor state. */
    struct SavedCursor {
        size::CellCountInt x;
        size::CellCountInt y;
        style::Style style;
        bool protected_;
        bool pending_wrap;
        bool origin;
        CharsetState charset;
    };

    struct Options {
        size::CellCountInt cols;
        size::CellCountInt rows;

        /* The maximum size of scrollback in bytes. Null is unlimited, zero
         * disables scrollback, and any other value is clamped to support a
         * minimum of the active area. */
        Maybe<size_t> max_scrollback_bytes; /* = 0 */

        /* The maximum number of physical scrollback rows, excluding the active
         * area. Null is unlimited. The effective limit permits at least one
         * standard page and only complete historical pages are pruned. */
        Maybe<size_t> max_scrollback_lines; /* = null */

        Options(size::CellCountInt c = 80, size::CellCountInt r = 24, Maybe<size_t> msb = (size_t)0,
                Maybe<size_t> msl = Maybe<size_t>())
            : cols(c), rows(r), max_scrollback_bytes(msb), max_scrollback_lines(msl) {}

        /* A simple, default terminal. If you rely on specific dimensions or
         * scrollback (or lack of) then do not use this directly. This is just
         * for callers that need some defaults. */
        static Options default_() { return Options(80, 24, (size_t)0); }
    };

    /* ------------------------------------------------------------------ */
    /* Fields                                                               */

    /* The general purpose allocator to use for all memory allocations.
     * Unfortunately some screen operations do require allocation. */
    zigstd::Allocator alloc;

    /* The list of pages in the screen. */
    PageList pages;

    /* Special-case where we want no scrollback whatsoever. We have to flag
     * this because max_size 0 in PageList gets rounded up to two pages so
     * we can always have an active screen. */
    bool no_scrollback; /* = false */

    /* The current cursor position */
    Cursor cursor;

    /* The saved cursor */
    Maybe<SavedCursor> saved_cursor; /* = null */

    /* The selection for this screen (if any). This MUST be a tracked selection
     * otherwise the selection will become invalid. Instead of accessing this
     * directly to set it, use the `select` function which will assert and
     * automatically setup tracking. */
    Maybe<Selection> selection; /* = null */

    /* The charset state */
    CharsetState charset;

    /* The current or most recent protected mode. Once a protection mode is
     * set, this will never become "off" again until the screen is reset.
     * The current state of whether protection attributes should be set is
     * set on the Cell pen; this is only used to determine the most recent
     * protection mode since some sequences such as ECH depend on this. */
    terminal::ansi::ProtectedMode protected_mode; /* = .off */

    /* The kitty keyboard settings. */
    terminal::kitty::KeyFlagStack kitty_keyboard;

    /* Semantic prompt (OSC133) state. */
    SemanticPrompt semantic_prompt;

    /* Dirty flags for the renderer. */
    Dirty dirty;

    Screen()
        : alloc(zigstd::c_allocator()), pages(), no_scrollback(false), cursor(), saved_cursor(), selection(),
          charset(), protected_mode(terminal::ansi::ProtectedMode::off), kitty_keyboard(),
          semantic_prompt(SemanticPrompt::disabled()), dirty() {}

    /* ------------------------------------------------------------------ */

    /* Initialize a new screen.
     *
     * max_scrollback_bytes is the amount of scrollback to keep in bytes. This
     * will be rounded UP to the nearest page size because our minimum allocation
     * size is that anyways.
     *
     * If max scrollback is 0, then no scrollback is kept at all.
     * Wisp: false is OutOfMemory. */
    static bool init(zigstd::Allocator alloc, const Options &opts, Screen *out) {
        /* Initialize our backing pages. */
        PageList pages;
        if (!PageList::init(alloc, PageList::Options(opts.cols, opts.rows, opts.max_scrollback_bytes,
                                                     opts.max_scrollback_lines),
                            &pages))
            return false;

        /* Create our tracked pin for the cursor. */
        Pin *page_pin = pages.trackPin(Pin(pages.pages.first));
        if (!page_pin) {
            pages.deinit();
            return false;
        }
        const Page::RowAndCell page_rac = page_pin->rowAndCell();

        Screen &result = *out;
        result = Screen();
        result.alloc = alloc;
        result.pages = pages;
        result.no_scrollback = opts.max_scrollback_bytes.has && opts.max_scrollback_bytes.value == 0;
        result.cursor.x = 0;
        result.cursor.y = 0;
        result.cursor.page_pin = page_pin;
        result.cursor.page_row = page_rac.row;
        result.cursor.page_cell = page_rac.cell;
        return true;
    }

    void deinit() {
        cursor.deinit(alloc);
        pages.deinit();
    }

    /* Assert that the screen is in a consistent state. This doesn't check
     * all pages in the page list because that is SO SLOW even just for
     * tests. This only asserts the screen specific data so callers should
     * ensure they're also calling page integrity checks if necessary. */
    void assertIntegrity() const {
        if (slow_runtime_safety) {
            assert(cursor.x < pages.cols);
            assert(cursor.y < pages.rows);

            /* Our cursor x/y should always match the pin. If this doesn't
             * match then it indicates that the tracked pin moved and we didn't
             * account for it by either calling cursorReload or manually
             * adjusting. */
            const Maybe<point::Point> pt = pages.pointFromPin(point::Tag::active, *cursor.page_pin);
            if (!pt.has || cursor.x != pt.value.c.x || cursor.y != pt.value.c.y) {
                fprintf(stderr, "Screen integrity: cursor/pin mismatch\n");
                abort();
            }

            /* The cursor style and hyperlink if non-zero must reference
             * real data in the page the pin is in. */
            const Page *page = cursor.page_pin->node->page();
            if (cursor.style_id != style::default_id) {
                assert(page->styles.refCount((const void *)page->memory, cursor.style_id) > 0);
            }
            if (cursor.hyperlink_id != 0) {
                assert(page->hyperlink_set.refCount((const void *)page->memory, cursor.hyperlink_id) > 0);
            }
        }
    }

    struct IntegrityGuard {
        const Screen *s;
        ~IntegrityGuard() { s->assertIntegrity(); }
    };

    /* Reset the screen according to the logic of a DEC RIS sequence.
     *
     * - Clears the screen and attempts to reclaim memory.
     * - Moves the cursor to the top-left.
     * - Clears any cursor state: style, hyperlink, etc.
     * - Resets the charset
     * - Clears the selection
     * - Deletes all Kitty graphics
     * - Resets Kitty Keyboard settings
     * - Disables protection mode */
    void reset() {
        /* Reset our pages */
        pages.reset();

        /* The above reset preserves tracked pins so we can still use
         * our cursor pin, which should be at the top-left already. The
         * reset marks every tracked pin as garbage, but we keep using
         * this one at its new valid position, so clear the flag: copies
         * of the cursor pin (e.g. for Kitty image placements) must not
         * be born garbage. */
        Pin *cursor_pin = cursor.page_pin;
        assert(cursor_pin->node == pages.pages.first);
        assert(cursor_pin->x == 0);
        assert(cursor_pin->y == 0);
        cursor_pin->garbage = false;
        const Page::RowAndCell cursor_rac = cursor_pin->rowAndCell();
        cursor.deinit(alloc);
        cursor = Cursor();
        cursor.page_pin = cursor_pin;
        cursor.page_row = cursor_rac.row;
        cursor.page_cell = cursor_rac.cell;

        /* Reset our basic state */
        saved_cursor = Maybe<SavedCursor>::none();
        charset = CharsetState();
        kitty_keyboard = terminal::kitty::KeyFlagStack();
        protected_mode = terminal::ansi::ProtectedMode::off;
        semantic_prompt = SemanticPrompt::disabled();
        clearSelection();
    }

    /* Clone the screen.
     *
     * This will copy:
     *
     *   - Screen dimensions
     *   - Screen data (cell state, etc.) for the region
     *
     * Anything not mentioned above is NOT copied. Some of this is for
     * very good reason:
     *
     *   - Kitty images have a LOT of data. This is not efficient to copy.
     *     Use a lock and access the image data. The dirty bit is there for
     *     a reason.
     *   - Cursor location can be expensive to calculate with respect to the
     *     specified region. It is faster to grab the cursor from the old
     *     screen and then move it to the new screen.
     *   - Current hyperlink cursor state has heap allocations. Since clone
     *     is only for read-only operations, it is better to not have any
     *     hyperlink state. Note that already-written hyperlinks are cloned.
     *
     * If not mentioned above, then there isn't a specific reason right now
     * to not copy some data other than we probably didn't need it and it
     * isn't necessary for screen coherency.
     *
     * Other notes:
     *
     *   - The viewport will always be set to the active area of the new
     *     screen. This is the bottom "rows" rows.
     *   - If the clone region is smaller than a viewport area, blanks will
     *     be filled in at the bottom.
     *
     * Wisp: `!Screen` error is a PageError. */
    page::PageError clone(zigstd::Allocator alloc_, const point::Point &top, Maybe<point::Point> bot,
                          Screen *out) const;

    PageList::IncreaseCapacityError increaseCapacity(PageList::Node *node,
                                                      Maybe<PageList::IncreaseCapacity> adjustment,
                                                      PageList::Node **out);

    /* Clone the cells in columns [x_start, x_end) of a source row into
     * the row at `dst_y` of the given node's page, increasing the node's
     * capacity as necessary to fit the managed memory (styles,
     * hyperlinks, etc.) of the copied cells.
     *
     * This is the Screen-level analog of PageList.cloneRowGrowCapacity:
     * capacity increases are routed through Screen.increaseCapacity so
     * that the cursor's style/hyperlink references are migrated when the
     * destination node is the cursor's page.
     *
     * Since increasing capacity replaces the node in the page list, the
     * (possibly replaced) node is returned and the caller must use it in
     * place of the old node. Tracked pins are updated automatically. The
     * source must NOT be on the given node since the node's page memory
     * may be freed on capacity increase.
     *
     * Callers use this mid-mutation (after rows have been rotated or
     * shifted), so a failure can't be propagated without leaving the
     * page list half-mutated (and corrupt). If the capacity can't be
     * increased (system OOM or the page is already at max capacity),
     * this panics: a crash is better than corruption. */
    PageList::Node *clonePartialRowGrowCapacity(PageList::Node *node, size_t dst_y, Page *src_page,
                                                const Row *src_row, size_t x_start, size_t x_end) {
        assert(src_page != node->page());

        PageList::Node *current = node;
        for (;;) {
            Page *cur_page = current->page();
            Row *cur_rows = cur_page->rows.ptr(cur_page->memory);
            const page::PageError err = cur_page->clonePartialRowFrom(src_page, &cur_rows[dst_y], src_row, x_start, x_end);
            if (err != page::PageError::none) {
                /* Adjust our page capacity to make room for what we
                 * didn't have space for and retry the copy. */
                PageList::Node *n;
                const PageList::IncreaseCapacityError e = increaseCapacity(current, PageList::forCloneError(err), &n);
                switch (e) {
                /* We can't gracefully recover from either of these
                 * here: our callers have already rotated or shifted
                 * rows, so returning an error would leave the page
                 * list half-mutated (and corrupt), so a crash is
                 * better. */
                case PageList::IncreaseCapacityError::OutOfMemory:
                    fprintf(stderr, "increaseCapacity system allocator OOM\n");
                    abort();
                case PageList::IncreaseCapacityError::OutOfSpace:
                    fprintf(stderr, "increaseCapacity OutOfSpace\n");
                    abort();
                default: break;
                }
                current = n;
                continue;
            }

            return current;
        }
    }

    Cell *cursorCellRight(size::CellCountInt n) {
        assert(cursor.x + n < pages.cols);
        return cursor.page_cell + n;
    }

    Cell *cursorCellLeft(size::CellCountInt n) {
        assert(cursor.x >= n);
        return cursor.page_cell - n;
    }

    Cell *cursorCellEndOfPrev() {
        assert(cursor.y > 0);

        Pin page_pin = cursor.page_pin->up(1).value;
        page_pin.x = (size::CellCountInt)(page_pin.node->cols() - 1);
        const Page::RowAndCell page_rac = page_pin.rowAndCell();
        return page_rac.cell;
    }

    /* Move the cursor right. This is a specialized function that is very fast
     * if the caller can guarantee we have space to move right (no wrapping). */
    void cursorRight(size::CellCountInt n) {
        assert(cursor.x + n < pages.cols);
        IntegrityGuard g = {this};

        cursor.page_cell = cursor.page_cell + n;
        cursor.page_pin->x += n;
        cursor.x += n;
    }

    /* Move the cursor left. */
    void cursorLeft(size::CellCountInt n) {
        assert(cursor.x >= n);
        IntegrityGuard g = {this};

        cursor.page_cell = cursor.page_cell - n;
        cursor.page_pin->x -= n;
        cursor.x -= n;
    }

    /* Move the cursor up.
     *
     * Precondition: The cursor is not at the top of the screen. */
    void cursorUp(size::CellCountInt n) {
        assert(cursor.y >= n);
        IntegrityGuard g = {this};

        cursor.y -= n; /* Must be set before cursorChangePin */
        cursorChangePin(cursor.page_pin->up(n).value);
        const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
        cursor.page_row = page_rac.row;
        cursor.page_cell = page_rac.cell;
    }

    Row *cursorRowUp(size::CellCountInt n) {
        assert(cursor.y >= n);
        IntegrityGuard g = {this};

        const Pin page_pin = cursor.page_pin->up(n).value;
        const Page::RowAndCell page_rac = page_pin.rowAndCell();
        return page_rac.row;
    }

    /* Move the cursor down.
     *
     * Precondition: The cursor is not at the bottom of the screen. */
    void cursorDown(size::CellCountInt n) {
        assert(cursor.y + n < pages.rows);
        IntegrityGuard g = {this};

        cursor.y += n; /* Must be set before cursorChangePin */

        /* We move the offset into our page list to the next row and then
         * get the pointers to the row/cell and set all the cursor state up. */
        cursorChangePin(cursor.page_pin->down(n).value);
        const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
        cursor.page_row = page_rac.row;
        cursor.page_cell = page_rac.cell;
    }

    /* Move the cursor to some absolute horizontal position. */
    void cursorHorizontalAbsolute(size::CellCountInt x) {
        assert(x < pages.cols);
        IntegrityGuard g = {this};

        cursor.page_pin->x = x;
        const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
        cursor.page_cell = page_rac.cell;
        cursor.x = x;
    }

    /* Move the cursor to some absolute position. */
    void cursorAbsolute(size::CellCountInt x, size::CellCountInt y) {
        assert(x < pages.cols);
        assert(y < pages.rows);
        IntegrityGuard g = {this};

        Pin page_pin;
        if (y < cursor.y) {
            page_pin = cursor.page_pin->up((size_t)cursor.y - y).value;
        } else if (y > cursor.y) {
            page_pin = cursor.page_pin->down((size_t)y - cursor.y).value;
        } else {
            page_pin = *cursor.page_pin;
        }
        page_pin.x = x;
        cursor.x = x; /* Must be set before cursorChangePin */
        cursor.y = y;
        cursorChangePin(page_pin);
        const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
        cursor.page_row = page_rac.row;
        cursor.page_cell = page_rac.cell;
    }

    /* Reloads the cursor pointer information into the screen. This is expensive
     * so it should only be done in cases where the pointers are invalidated
     * in such a way that its difficult to recover otherwise. */
    void cursorReload() {
        IntegrityGuard g = {this};

        /* Our tracked pin is ALWAYS accurate, so we derive the active
         * point from the pin. If this returns null it means our pin
         * points outside the active area. In that case, we update the
         * pin to be the top-left. */
        Maybe<point::Point> pt_ = pages.pointFromPin(point::Tag::active, *cursor.page_pin);
        point::Point pt;
        if (pt_.has) {
            pt = pt_.value;
        } else {
            /* Our cached row/cell pointers may be invalid (that is often
             * the reason cursorReload is being called), so refresh them
             * from the pin first since cursorChangePin below marks the
             * old cursor row as dirty. */
            const Page::RowAndCell old_rac = cursor.page_pin->rowAndCell();
            cursor.page_row = old_rac.row;
            cursor.page_cell = old_rac.cell;

            /* The cursor style and hyperlink IDs are only valid within the
             * page that the pin points at, so the pin change must go through
             * cursorChangePin, which migrates them when the active top-left
             * is on a different page. Writing the pin directly here would
             * leave the cursor holding IDs that are dead or alias unrelated
             * entries on the new page. */
            const Pin p = pages.pin(point::Point::active()).value;
            cursor.x = 0; /* Must be set before cursorChangePin */
            cursor.y = 0;
            cursorChangePin(p);

            /* cursorChangePin can trigger a page capacity adjustment which
             * moves the pin again, so we re-read it to derive our point. */
            pt = pages.pointFromPin(point::Tag::active, *cursor.page_pin).value;
        }

        cursor.x = (size::CellCountInt)pt.c.x;
        cursor.y = (size::CellCountInt)pt.c.y;
        const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
        cursor.page_row = page_rac.row;
        cursor.page_cell = page_rac.cell;
    }

    /* Scroll the active area and keep the cursor at the bottom of the screen.
     * This is a very specialized function but it keeps it fast.
     * Wisp: false is OutOfMemory. */
    bool cursorDownScroll();

    /* This scrolls the active area at and above the cursor.
     * The lines below the cursor are not scrolled. */
    bool cursorScrollAbove();
    bool cursorScrollAboveRotate(PageList::Node *fresh_node);

    /* Scroll a full-width scroll region that ends at the cursor row up by
     * one row. The cursor must be on the bottom row of the region. `limit`
     * is the number of rows in the region above the cursor (region height
     * minus one) and must be at least 1.
     *
     * The top row of the region is discarded (NOT moved into scrollback).
     * All other rows in the region shift up by one and the cursor row
     * becomes a blank row, filled with the current background color (like
     * other scroll operations such as deleteLines).
     *
     * The cursor stays at the same screen position (the new blank row).
     * Content outside of the region is unmodified.
     *
     * This is a very hot path for scroll region usage (e.g. a program
     * on the alt screen using DECSTBM and scrolling via LF/IND) so this
     * is optimized for the common case where the full region is within
     * a single page. */
    bool cursorScrollRegionUp(size_t limit);

    /* Slow path for cursorScrollRegionUp: the scroll region spans
     * multiple pages so we use the generic PageList erase machinery. */
    bool cursorScrollRegionUpSlow(size_t limit);

    /* Move the cursor down if we're not at the bottom of the screen. Otherwise
     * scroll. Currently only used for testing. */
    bool cursorDownOrScroll() {
        if ((size_t)cursor.y + 1 < pages.rows) {
            cursorDown(1);
            return true;
        }
        return cursorDownScroll();
    }

    /* Copy another cursor. The cursor can be on any screen but the x/y
     * must be within our screen bounds. */
    PageList::IncreaseCapacityError cursorCopy(const Cursor &other, bool copy_hyperlink = true);

    /* Always use this to write to cursor.page_pin.*.
     *
     * This specifically handles the case when the new pin is on a different
     * page than the old AND we have a style or hyperlink set. In that case,
     * we must release our old one and insert the new one, since styles are
     * stored per-page.
     *
     * Note that this can change the cursor pin AGAIN if the process of
     * setting up our cursor forces a capacity adjustment of the underlying
     * cursor page, so any references to the page pin should be re-read
     * from `self.cursor.page_pin` after calling this. */
    void cursorChangePin(const Pin &new_);

    /* Mark the cursor position as dirty.
     * TODO: test */
    void cursorMarkDirty() { cursor.page_row->setDirty(true); }

    /* Reset the cursor row's soft-wrap state and the cursor's pending wrap.
     * Also handles clearing the spacer head on the cursor row and resetting
     * the wrap_continuation flag on the next row if necessary.
     *
     * NOTE(qwerasd): This method is not scrolling region aware, and cannot be
     * since it's on Screen not Terminal. This needs to be addressed down the
     * line. Not an extremely urgent issue since it's an edge case of an edge
     * case, but not ideal. */
    void cursorResetWrap() {
        /* Reset the cursor's pending wrap state */
        cursor.pending_wrap = false;

        Row *page_row = cursor.page_row;

        if (!page_row->wrap()) return;

        /* This row does not wrap and the next row is not wrapped to */
        page_row->setWrap(false);

        const Maybe<Pin> next_row = cursor.page_pin->down(1);
        if (next_row.has) {
            next_row.value.rowAndCell().row->setWrapContinuation(false);
        }

        /* If the last cell in the row is a spacer head we need to clear it. */
        size_t n;
        Cell *cells = cursor.page_pin->cells(Pin::CellSubset::all, &n);
        const size_t last = (size_t)cursor.page_pin->node->cols() - 1;
        if (cells[last].wide() == Cell::Wide::spacer_head) {
            clearCells(cursor.page_pin->node->page(), page_row, cells + last, 1);
        }
    }

    /* Options for scrolling the viewport of the terminal grid. The reason
     * we have this in addition to PageList.Scroll is because we have additional
     * scroll behaviors that are not part of the PageList.Scroll enum.
     * Wisp: identical tags, so it is PageList::Scroll. */
    typedef PageList::Scroll Scroll;

    /* Scroll the viewport of the terminal grid. */
    void scroll(const Scroll &behavior) {
        IntegrityGuard g = {this};
        pages.scroll(behavior);
    }

    /* See PageList.scrollClear. In addition to that, we reset the cursor
     * to be on top. */
    bool scrollClear() {
        IntegrityGuard g = {this};

        if (!pages.scrollClear()) return false;
        cursorReload();
        return true;
    }

    /* Returns true if the viewport is scrolled to the bottom of the screen. */
    bool viewportIsBottom() const { return pages.viewport == PageList::Viewport::active; }

    /* Erase the region specified by tl and br, inclusive. This will physically
     * erase the rows meaning the memory will be reclaimed (if the underlying
     * page is empty) and other rows will be shifted up. */
    void eraseHistory(Maybe<point::Point> bl) {
        IntegrityGuard g = {this};
        pages.eraseHistory(bl);
        cursorReload();
    }

    void eraseActive(size::CellCountInt y) {
        IntegrityGuard g = {this};
        pages.eraseActive(y);
        cursorReload();
    }

    /* Clear the region specified by tl and bl, inclusive. Cleared cells are
     * colored with the current style background color. This will clear all
     * cells in the rows.
     *
     * If protected is true, the protected flag will be respected and only
     * unprotected cells will be cleared. Otherwise, all cells will be cleared. */
    void clearRows(const point::Point &tl, Maybe<point::Point> bl, bool protected_) {
        IntegrityGuard g = {this};

        PageList::PageIterator it = pages.pageIterator(PageList::Direction::right_down, tl, bl);
        PageList::Chunk chunk;
        while (it.next(&chunk)) {
            Page *page = chunk.node->page();
            size_t n;
            Row *rows = chunk.rows(&n);
            for (size_t i = 0; i < n; i++) {
                Row *row = &rows[i];
                const size::Offset<Cell> cells_offset = row->cells();
                Cell *cells = page->getCells(row);

                /* Clear all cells */
                if (protected_) {
                    clearUnprotectedCells(page, row, cells, page->size.cols);
                    /* We need to preserve other row attributes since we only
                     * cleared unprotected cells. */
                    row->setCells(cells_offset);
                } else {
                    clearCells(page, row, cells, page->size.cols);
                    *row = Row::withCells(cells_offset);
                }

                row->setDirty(true);
            }
        }
    }

    /* Clear the cells with the blank cell.
     *
     * This takes care to handle cleaning up graphemes and styles. */
    void clearCells(Page *page, Row *row, Cell *cells, size_t cells_len) {
        if (cells_len == 0) return;

        /* This whole operation does unsafe things, so we just want to assert
         * the end state. */
        page->pauseIntegrityChecks(true);
        struct Guard {
            Screen *s;
            Page *page;
            ~Guard() {
                page->pauseIntegrityChecks(false);
                page->assertIntegrity();
                s->assertIntegrity();
            }
        } guard = {this, page};

        if (slow_runtime_safety) {
            /* Our row and cells should be within the page. */
            Row *page_rows = page->rows.ptr(page->memory);
            assert(row >= &page_rows[0]);
            assert(row <= &page_rows[page->size.rows - 1]);

            Cell *row_cells = page->getCells(row);
            assert(&cells[0] >= &row_cells[0]);
            assert(&cells[cells_len - 1] <= &row_cells[page->size.cols - 1]);
        }

        /* If we have managed memory (styles, graphemes, or hyperlinks)
         * in this row then we go cell by cell and clear them if present. */
        if (row->grapheme()) {
            for (size_t i = 0; i < cells_len; i++) {
                if (cells[i].hasGrapheme()) page->clearGrapheme(&cells[i]);
            }

            /* If we have no left/right scroll region we can be sure
             * that we've cleared all the graphemes, so we clear the
             * flag, otherwise we ask the page to update the flag. */
            if (cells_len == page->size.cols) {
                row->setGrapheme(false);
            } else {
                page->updateRowGraphemeFlag(row);
            }
        }

        if (row->hyperlink()) {
            for (size_t i = 0; i < cells_len; i++) {
                if (cells[i].hyperlink()) page->clearHyperlink(&cells[i]);
            }

            /* If we have no left/right scroll region we can be sure
             * that we've cleared all the hyperlinks, so we clear the
             * flag, otherwise we ask the page to update the flag. */
            if (cells_len == page->size.cols) {
                row->setHyperlink(false);
            } else {
                page->updateRowHyperlinkFlag(row);
            }
        }

        if (row->styled()) {
            /* Styled cells overwhelmingly come in runs sharing the same
             * style (e.g. a colored status bar or a highlighted region),
             * so group them and release each run with a single ref-count
             * update rather than per cell. */
            size_t i = 0;
            while (i < cells_len) {
                const style::Id id = cells[i].style_id();
                if (id == style::default_id) {
                    i += 1;
                    continue;
                }
                size_t j = i + 1;
                while (j < cells_len && cells[j].style_id() == id) j += 1;
                page->styles.releaseMultiple((const void *)page->memory, id, (style::Id)(j - i));
                i = j;
            }

            /* If we have no left/right scroll region we can be sure
             * that we've cleared all the styles, so we clear the
             * flag, otherwise we ask the page to update the flag. */
            if (cells_len == page->size.cols) {
                row->setStyled(false);
            } else {
                page->updateRowStyledFlag(row);
            }
        }

        if (row->kitty_virtual_placeholder() && cells_len == page->size.cols) {
            bool found = false;
            for (size_t i = 0; i < cells_len; i++) {
                if (cells[i].codepoint() == kitty_placeholder) {
                    found = true;
                    break;
                }
            }
            if (!found) row->setKittyVirtualPlaceholder(false);
        }

        const Cell blank = blankCell();
        for (size_t i = 0; i < cells_len; i++) cells[i] = blank;
    }

    /* Clear cells but only if they are not protected. */
    void clearUnprotectedCells(Page *page, Row *row, Cell *cells, size_t cells_len) {
        size_t x0 = 0;
        size_t x1 = 0;

        while (x0 < cells_len) {
            bool done = false;
            while (cells[x0].protected_()) {
                x0 += 1;
                if (x0 >= cells_len) {
                    done = true;
                    break;
                }
            }
            if (done) break;
            x1 = x0 + 1;
            while (x1 < cells_len && !cells[x1].protected_()) {
                x1 += 1;
            }
            clearCells(page, row, cells + x0, x1 - x0);
            x0 = x1;
        }

        page->assertIntegrity();
        assertIntegrity();
    }

    /* Clean up boundary conditions where a cell will become discontiguous with
     * a neighboring cell because either one of them will be moved and/or cleared.
     *
     * For performance reasons this is specialized to operate on the cursor row.
     *
     * Handles the boundary between the cell at `x` and the cell at `x - 1`.
     *
     * So, for example, when moving a region of cells [a, b] (inclusive), call this
     * function with `x = a` and `x = b + 1`. It is okay if `x` is out of bounds by
     * 1, this will be interpreted correctly.
     *
     * DOES NOT MODIFY ROW WRAP STATE! See `cursorResetWrap` for that.
     *
     * The following boundary conditions are handled:
     *
     * - `x - 1` is a wide character and `x` is a spacer tail:
     *   o Both cells will be cleared.
     *   o If `x - 1` is the start of the row and was wrapped from a previous row
     *     then the previous row is checked for a spacer head, which is cleared if
     *     present.
     *
     * - `x == 0` and is a wide character:
     *   o If the row is a wrap continuation then the previous row will be checked
     *     for a spacer head, which is cleared if present.
     *
     * - `x == cols` and `x - 1` is a spacer head:
     *   o `x - 1` will be cleared.
     *
     * NOTE(qwerasd): This method is not scrolling region aware, and cannot be
     * since it's on Screen not Terminal. This needs to be addressed down the
     * line. Not an extremely urgent issue since it's an edge case of an edge
     * case, but not ideal. */
    void splitCellBoundary(size::CellCountInt x) {
        Page *page = cursor.page_pin->node->page();

        page->pauseIntegrityChecks(true);
        struct Guard {
            Page *p;
            ~Guard() { p->pauseIntegrityChecks(false); }
        } guard = {page};

        const size::CellCountInt cols = cursor.page_pin->node->cols();

        /* `x` may be up to an INCLUDING `cols`, since that signifies splitting
         * the boundary to the right of the final cell in the row. */
        assert(x <= cols);

        size_t n;

        /* [ A B C D E F|]
         *              ^ Boundary between final cell and row end. */
        if (x == cols) {
            if (!cursor.page_row->wrap()) return;

            Cell *cells = cursor.page_pin->cells(Pin::CellSubset::all, &n);

            /* Spacer head at end of wrapped row. */
            if (cells[cols - 1].wide() == Cell::Wide::spacer_head) {
                clearCells(page, cursor.page_row, cells + cols - 1, 1);
            }

            return;
        }

        /* [|A B C D E F ]
         *  ^ Boundary between first cell and row start.
         *
         *  OR
         *
         * [ A|B C D E F ]
         *    ^ Boundary between first cell and second cell.
         *
         * First cell may be a wrapped wide cell with a spacer
         * head on the previous row that needs to be cleared. */
        if ((x == 0 || x == 1) && cursor.page_row->wrap_continuation()) {
            Cell *cells = cursor.page_pin->cells(Pin::CellSubset::all, &n);

            /* If the first cell in a row is wide the previous row
             * may have a spacer head which needs to be cleared. */
            if (cells[0].wide() == Cell::Wide::wide) {
                const Maybe<Pin> p_row_ = cursor.page_pin->up(1);
                if (p_row_.has) {
                    const Pin p_row = p_row_.value;
                    const Page::RowAndCell p_rac = p_row.rowAndCell();
                    size_t pn;
                    Cell *p_cells = p_row.cells(Pin::CellSubset::all, &pn);
                    const size_t last = (size_t)p_row.node->cols() - 1;
                    if (p_cells[last].wide() == Cell::Wide::spacer_head) {
                        clearCells(p_row.node->page(), p_rac.row, p_cells + last, 1);

                        /* `clearCells` does not mark rows dirty, and our
                         * callers only mark the cursor row, so mark the
                         * previous row here. */
                        p_row.markDirty();
                    }
                }
            }
        }

        /* If x is 0 then we're done. */
        if (x == 0) return;

        /* [ ... X|Y ... ]
         *        ^ Boundary between two cells in the middle of the row. */
        {
            assert(x > 0);
            assert(x < cols);

            Cell *cells = cursor.page_pin->cells(Pin::CellSubset::all, &n);

            const Cell left = cells[x - 1];
            switch (left.wide()) {
            /* There should not be spacer heads in the middle of the row. */
            case Cell::Wide::spacer_head: assert(false); break;

            /* We don't need to do anything for narrow cells or spacer tails. */
            case Cell::Wide::narrow:
            case Cell::Wide::spacer_tail: break;

            /* A wide char would be split, so must be cleared. */
            case Cell::Wide::wide: clearCells(page, cursor.page_row, cells + x - 1, 2); break;
            }
        }
    }

    /* Returns the blank cell to use when doing terminal operations that
     * require preserving the bg color. */
    Cell blankCell() const {
        if (cursor.style_id == style::default_id) return Cell();
        Cell c;
        if (cursor.style.bgCell(&c)) return c;
        return Cell();
    }

    struct Resize {
        /* The new size to resize to */
        size::CellCountInt cols;
        size::CellCountInt rows;

        /* Whether to reflow soft-wrapped text.
         *
         * This will reflow soft-wrapped text. If the screen size is getting
         * smaller and the maximum scrollback size is exceeded, data will be
         * lost from the top of the scrollback. */
        bool reflow; /* = true */

        /* Set this to enable prompt redraw on resize. This signals
         * that the running program can redraw the prompt if the cursor is
         * currently at a prompt. This detects OSC133 prompts lines and clears
         * them. If set to `.last`, only the most recent prompt line is cleared. */
        terminal::osc::semantic_prompt::Redraw prompt_redraw; /* = .false */

        /* Whether the resize may pull rows out of scrollback back into the
         * active area. See PageList.Resize for details. */
        bool pull_scrollback; /* = true */

        Resize(size::CellCountInt c, size::CellCountInt r, bool rf = true)
            : cols(c), rows(r), reflow(rf), prompt_redraw(terminal::osc::semantic_prompt::Redraw::false_),
              pull_scrollback(true) {}
    };

    enum class ResizeTw { saved_cursor_pin, pages };
    typedef tripwire::Module<ResizeTw, AllocTw, 2> resize_tw;

    /* Resize the screen. The rows or cols can be bigger or smaller. If this
     * returns an error, the screen is unchanged. Wisp: false is OutOfMemory. */
    bool resize(const Resize &opts);

    void clearPromptForRedraw(terminal::osc::semantic_prompt::Redraw redraw);

    /* Set a style attribute for the current cursor.
     *
     * If the style can't be set due to any internal errors (memory-related),
     * then this will revert back to the existing style and return an error. */
    PageList::IncreaseCapacityError setAttribute(const terminal::sgr::Attribute &attr);

    /* Call this whenever you manually change the cursor style.
     *
     * This function can NOT fail if the cursor style is changing to the
     * default style.
     *
     * If this returns an error, the style change did not take effect and
     * the cursor style is reverted back to the default. The only scenario
     * this returns an error is if there is a physical memory allocation failure
     * or if there is no possible way to increase style capacity to store
     * the style.
     *
     * This function WILL split pages as necessary to accommodate the new style.
     * So if OutOfSpace is returned, it means that even after splitting the page
     * there was still no room for the new style. */
    PageList::IncreaseCapacityError manualStyleUpdate();

    /* Split at the given pin so that the pinned row moves to the page
     * with less used capacity after the split.
     *
     * The primary use case for this is to handle IncreaseCapacityError
     * OutOfSpace conditions where we need to split the page in order
     * to make room for more managed memory.
     *
     * If the caller cares about where the pin moves to, they should
     * setup a tracked pin before calling this and then check that.
     * In many calling cases, the input pin is tracked (e.g. the cursor
     * pin).
     *
     * If this returns OOM then its a system OOM. If this returns OutOfSpace
     * then it means the page can't be split further. */
    PageList::SplitError splitForCapacity(const Pin &pin);

    /* Append a grapheme to the given cell within the current cursor row. */
    PageList::IncreaseCapacityError appendGrapheme(Cell *cell, uint32_t cp);

    /* Start the hyperlink state. Future cells will be marked as hyperlinks with
     * this state. Note that various terminal operations may clear the hyperlink
     * state, such as switching screens (alt screen).
     * Wisp: `id_` null is id == nullptr. */
    PageList::IncreaseCapacityError startHyperlink(const uint8_t *uri, size_t uri_len, const uint8_t *id,
                                                   size_t id_len);

    /* Wisp: Allocator.Error || Page.InsertHyperlinkError */
    page::PageError startHyperlinkOnce(const hyperlink::Hyperlink &source);

    /* End the hyperlink state so that future cells aren't part of the
     * current hyperlink (if any). This is safe to call multiple times. */
    void endHyperlink() {
        /* If we have no hyperlink state then do nothing */
        if (cursor.hyperlink_id == 0) {
            assert(cursor.hyperlink == nullptr);
            return;
        }

        /* Release the old hyperlink state. If there are cells using the
         * hyperlink this will work because the creation creates a reference
         * and all additional cells create a new reference. This release will
         * just release our initial reference.
         *
         * If the ref count reaches zero the set will not delete the item
         * immediately; it is kept around in case it is used again (this is
         * how RefCountedSet works). This causes some memory fragmentation but
         * is fine because if it is ever pruned the context deleted callback
         * will be called. */
        Page *page = cursor.page_pin->node->page();
        page->hyperlink_set.release((const void *)page->memory, cursor.hyperlink_id);
        cursor.hyperlink->deinit(alloc);
        alloc.destroy(cursor.hyperlink);
        cursor.hyperlink_id = 0;
        cursor.hyperlink = nullptr;
    }

    /* Set the current hyperlink state on the current cell. */
    PageList::IncreaseCapacityError cursorSetHyperlink();

    /* Modify the semantic content type of the cursor. This should
     * be preferred over setting it manually since it handles all the
     * proper accounting. */
    struct SemanticContentSet {
        enum class Tag { prompt, output, input } tag;
        terminal::osc::semantic_prompt::PromptKind prompt;
        enum class InputClear { clear_explicit, clear_eol } input;

        static SemanticContentSet makePrompt(terminal::osc::semantic_prompt::PromptKind k) {
            SemanticContentSet s;
            s.tag = Tag::prompt;
            s.prompt = k;
            s.input = InputClear::clear_explicit;
            return s;
        }
        static SemanticContentSet makeOutput() {
            SemanticContentSet s;
            s.tag = Tag::output;
            s.prompt = terminal::osc::semantic_prompt::PromptKind::initial;
            s.input = InputClear::clear_explicit;
            return s;
        }
        static SemanticContentSet makeInput(InputClear c) {
            SemanticContentSet s;
            s.tag = Tag::input;
            s.prompt = terminal::osc::semantic_prompt::PromptKind::initial;
            s.input = c;
            return s;
        }
    };

    void cursorSetSemanticContent(const SemanticContentSet &t) {
        typedef terminal::osc::semantic_prompt::PromptKind PK;
        switch (t.tag) {
        case SemanticContentSet::Tag::output:
            cursor.semantic_content = Cell::SemanticContent::output;
            cursor.semantic_content_clear_eol = false;
            break;

        case SemanticContentSet::Tag::input:
            cursor.semantic_content = Cell::SemanticContent::input;
            cursor.semantic_content_clear_eol = t.input == SemanticContentSet::InputClear::clear_eol;
            break;

        case SemanticContentSet::Tag::prompt:
            semantic_prompt.seen = true;
            cursor.semantic_content = Cell::SemanticContent::prompt;
            cursor.semantic_content_clear_eol = false;
            cursor.page_row->setSemanticPrompt((t.prompt == PK::initial || t.prompt == PK::right)
                                                   ? Row::SemanticPrompt::prompt
                                                   : Row::SemanticPrompt::prompt_continuation);
            break;
        }
    }

    /* Set the selection to the given selection. If this is a tracked selection
     * then the screen will take ownership of the selection. If this is untracked
     * then the screen will convert it to tracked internally. This will automatically
     * untrack the prior selection (if any).
     *
     * Set the selection to null to clear any previous selection.
     *
     * This is always recommended over setting `selection` directly. Beyond
     * managing memory for you, it also performs safety checks that the selection
     * is always tracked. Wisp: false is OutOfMemory. */
    bool select(Maybe<Selection> sel_) {
        if (!sel_.has) {
            clearSelection();
            return true;
        }
        const Selection sel = sel_.value;

        /* If this selection is untracked then we track it. */
        Selection tracked_sel;
        if (sel.tracked()) {
            tracked_sel = sel;
        } else if (!sel.track(this, &tracked_sel)) {
            return false;
        }

        /* Untrack prior selection pins that aren't also owned by the replacement.
         * A caller may pass our current tracked selection back to us by value, so
         * releasing both old pins unconditionally would leave the replacement
         * pointing at freed pool entries. */
        if (selection.has) {
            const Selection old = selection.value;
            if (old.bounds.tag == Selection::Bounds::Tag::untracked) {
                old.deinit(this);
            } else {
                Pin *os = old.bounds.tracked.start;
                Pin *oe = old.bounds.tracked.end;
                Pin *ns = tracked_sel.bounds.tracked.start;
                Pin *ne = tracked_sel.bounds.tracked.end;
                if (os != ns && os != ne) pages.untrackPin(os);
                if (oe != ns && oe != ne) pages.untrackPin(oe);
            }
        }
        selection = tracked_sel;
        dirty.selection = true;
        return true;
    }

    /* Same as select(null) but can't fail. */
    void clearSelection() {
        if (selection.has) {
            selection.value.deinit(this);
            dirty.selection = true;
        }
        selection = Maybe<Selection>::none();
    }

    struct SelectionString {
        /* The selection to convert to a string. */
        Selection sel;

        /* If true, trim whitespace around the selection. */
        bool trim; /* = true */

        explicit SelectionString(const Selection &s, bool t = true) : sel(s), trim(t) {}
    };

    /* Returns the raw text associated with a selection. This will unwrap
     * soft-wrapped edges. The returned slice is owned by the caller and allocated
     * using alloc, not the allocator associated with the screen (unless they match).
     *
     * For more flexibility, use a ScreenFormatter directly. */
    std::string selectionString(const SelectionString &opts);

    struct SelectLine {
        /* The pin of some part of the line to select. */
        Pin pin;

        /* These are the codepoints to consider whitespace to trim
         * from the ends of the selection. Wisp: null is whitespace_len
         * == SIZE_MAX. */
        const uint32_t *whitespace;
        size_t whitespace_len;

        /* If true, line selection will consider semantic prompt
         * state changing a boundary. State changing is ANY state
         * change. */
        bool semantic_prompt_boundary; /* = true */

        explicit SelectLine(const Pin &p);
    };

    /* Select the line under the given point. This will select across soft-wrapped
     * lines and will omit the leading and trailing whitespace. If the point is
     * over whitespace but the line has non-whitespace characters elsewhere, the
     * line will be selected. */
    Maybe<Selection> selectLine(const SelectLine &opts) const;

    /* Return the selection for all contents on the screen. Surrounding
     * whitespace is omitted. If there is no selection, this returns null. */
    Maybe<Selection> selectAll();

    /* Select the nearest word to start point that is between start_pt and
     * end_pt (inclusive). Because it selects "nearest" to start point, start
     * point can be before or after end point.
     *
     * The boundary_codepoints parameter should be a slice of u21 codepoints that
     * mark word boundaries, passed through to selectWord.
     *
     * TODO: test this */
    Maybe<Selection> selectWordBetween(const Pin &start, const Pin &end, const uint32_t *boundary_codepoints,
                                       size_t boundary_len) {
        const PageList::Direction dir =
            start.before(end) ? PageList::Direction::right_down : PageList::Direction::left_up;
        PageList::CellIterator it = start.cellIterator(dir, end);
        Pin p;
        while (it.next(&p)) {
            /* Boundary conditions */
            switch (dir) {
            case PageList::Direction::right_down:
                if (end.before(p)) return Maybe<Selection>::none();
                break;
            case PageList::Direction::left_up:
                if (p.before(end)) return Maybe<Selection>::none();
                break;
            }

            /* If we found a word, then return it */
            const Maybe<Selection> sel = selectWord(p, boundary_codepoints, boundary_len);
            if (sel.has) return sel;
        }

        return Maybe<Selection>::none();
    }

    /* Select the word under the given point. A word is any consecutive series
     * of characters that are exclusively whitespace or exclusively non-whitespace.
     * A selection can span multiple physical lines if they are soft-wrapped.
     *
     * This will return null if a selection is impossible. The only scenario
     * this happens is if the point pt is outside of the written screen space.
     *
     * The boundary_codepoints parameter should be a slice of u21 codepoints that
     * mark word boundaries. This is expected to be pre-parsed from the config. */
    Maybe<Selection> selectWord(const Pin &pin, const uint32_t *boundary_codepoints, size_t boundary_len);

    /* Select the command output under the given point. The limits of the output
     * are determined by semantic prompt information provided by shell integration.
     * A selection can span multiple physical lines if they are soft-wrapped.
     *
     * This will return null if a selection is impossible:
     *  - the point pt is outside of the written screen space.
     *  - the point pt is on a prompt / input line. */
    Maybe<Selection> selectOutput(const Pin &pin);

    struct LineIterator {
        const Screen *screen;
        Maybe<Pin> current; /* = null */

        bool next(Selection *out);
    };

    /* Returns an iterator to move through the soft-wrapped lines starting
     * from pin. */
    LineIterator lineIterator(const Pin &start) const {
        LineIterator it;
        it.screen = this;
        it.current = start;
        return it;
    }

    struct PromptClickMove {
        size_t left;
        size_t right;

        static PromptClickMove zero() {
            PromptClickMove m = {0, 0};
            return m;
        }
    };

    /* Determine the inputs necessary to move the cursor to the given
     * click location within a prompt input area.
     *
     * If the cursor isn't currently at a prompt input location, this
     * returns no movement.
     *
     * This feature depends on well-behaved OSC133 shell integration. Specifically,
     * this only moves over designated input areas (OSC 133 B). It is assumed
     * that the shell will only move the cursor to input cells, so prompt cells
     * and other blank cells are ignored as part of the movement calculation. */
    PromptClickMove promptClickMove(const Pin &click_pin) {
        /* If we're not at an input cell with our cursor, no movement will
         * ever be possible. */
        if (cursor.semantic_content != Cell::SemanticContent::input &&
            cursor.page_cell->semantic_content() != Cell::SemanticContent::input)
            return PromptClickMove::zero();

        switch (semantic_prompt.click.tag) {
        /* None doesn't support movement and click_events must use a
         * different mechanism (SGR mouse events) that callers must handle. */
        case SemanticPrompt::SemanticClick::Kind::none:
        case SemanticPrompt::SemanticClick::Kind::click_events: return PromptClickMove::zero();
        /* All of these currently use dumb line-based navigation.
         * But eventually we'll support more. */
        default: return promptClickLine(click_pin);
        }
    }

    /* Determine the inputs required to move from the cursor to the given
     * click location. If the cursor isn't currently at a prompt input
     * location, this will return zero.
     *
     * This currently only supports moving a single line. */
    PromptClickMove promptClickLine(const Pin &click_pin);

    struct DumpString {
        /* The start and end points of the dump, both inclusive. The x will
         * be ignored and the full row will always be dumped. */
        Pin tl;
        Maybe<Pin> br; /* = null */

        /* If true, this will unwrap soft-wrapped lines. If false, this will
         * dump the screen as it is visually seen in a rendered window. */
        bool unwrap; /* = true */
    };

    /* Dump the screen to a string. The writer given should be buffered;
     * this function does not attempt to efficiently write and generally writes
     * one byte at a time. */
    void dumpString(std::string *writer, const DumpString &opts) const;

    /* You should use dumpString, this is a restricted version mostly for
     * legacy and convenience reasons for unit tests. */
    std::string dumpStringAlloc(const point::Point &tl) const;

    /* You should use dumpString, this is a restricted version mostly for
     * legacy and convenience reasons for unit tests. */
    std::string dumpStringAllocUnwrapped(const point::Point &tl) const;

    /* This is basically a really jank version of Terminal.printString. We
     * have to reimplement it here because we want a way to print to the screen
     * to test it but don't want all the features of Terminal. */
    bool testWriteString(const char *text);
};

} /* namespace vt */
} /* namespace wisp */

#include "screen_impl.hpp"

#endif /* WISP_VT_SCREEN_HPP */
