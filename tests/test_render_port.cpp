/* Transliterated from the test blocks in Ghostty src/terminal/render.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 *
 * Wisp: `t.vtStream()` is a stream_terminal.Handler wrapped in a Stream,
 * the shape the other Terminal-driving test ports use. `std.MultiArrayList`
 * field slices are one array of structs (see src/vt/render.hpp), so
 * `row_data.items(.cells)[y]` reads `row_data.items[y].cells`.
 */

#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../vt/render.hpp"
#include "../terminal/stream_terminal.hpp"
#include "../zigstd/random.hpp"

using namespace wisp;
using namespace wisp::vt;

namespace st = wisp::terminal::stream_terminal;

typedef render::RenderState RenderState;
typedef PageList::Pin Pin;
typedef style::Style Style;
typedef st::Handler Handler;
typedef wisp::terminal::stream::Stream<Handler> Stream;
typedef zigstd::DefaultPrng DefaultPrng;
typedef zigstd::Random Random;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: `var t = try Terminal.init(io, alloc, .{...}); defer t.deinit(alloc);` */
struct TermHolder {
    Terminal t;
    bool ok;
    explicit TermHolder(const Terminal::Options &o) { ok = Terminal::init(talloc(), o, &t); }
    ~TermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: `var s = t.vtStream(); defer s.deinit();` */
struct StreamHolder {
    Stream s;
    explicit StreamHolder(Terminal *t) : s(Handler::init(t), opts()) {}
    ~StreamHolder() { s.deinit(); }
    static Stream::Options opts() {
        Stream::Options o;
        o.allocator = true;
        return o;
    }
};

/* Wisp: `var state: RenderState = .empty; defer state.deinit(alloc);` */
struct StateHolder {
    RenderState s;
    StateHolder() : s(RenderState::empty()) {}
    ~StateHolder() { s.deinit(talloc()); }
};

#define TERM(v, c, r)                                                                              \
    Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                  \
    TermHolder v##_holder(v##_opts);                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    Terminal &v = v##_holder.t

#define STREAM(v, t)                                                                               \
    StreamHolder v##_holder(&(t));                                                                 \
    Stream &v = v##_holder.s

#define STATE(v)                                                                                   \
    StateHolder v##_holder;                                                                        \
    RenderState &v = v##_holder.s

/* ─────────────────────────────────────────────────────────────────────── */

TEST(render, styled) {
    TERM(t, 80, 24);

    /* This fills the screen up */
    ASSERT_TRUE(t.decaln());

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));
}

TEST(render, basic_text) {
    TERM(t, 10, 3);

    STREAM(s, t);
    s.nextSlice("ABCD");

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Verify we have the right number of rows */
    RenderState::Row *row_data = state.row_data.items;
    ASSERT_TRUE(3 == state.row_data.items_len);

    /* All rows should have cols cells */
    ASSERT_TRUE(10 == row_data[0].cells.items_len);
    ASSERT_TRUE(10 == row_data[1].cells.items_len);
    ASSERT_TRUE(10 == row_data[2].cells.items_len);

    /* Row zero should contain our text */
    ASSERT_TRUE('A' == row_data[0].cells.items[0].raw.codepoint());
    ASSERT_TRUE('B' == row_data[0].cells.items[1].raw.codepoint());
    ASSERT_TRUE('C' == row_data[0].cells.items[2].raw.codepoint());
    ASSERT_TRUE('D' == row_data[0].cells.items[3].raw.codepoint());
    ASSERT_TRUE(0 == row_data[0].cells.items[4].raw.codepoint());
}

TEST(render, styled_text) {
    TERM(t, 10, 3);

    STREAM(s, t);
    s.nextSlice("\x1b[1mA");    /* Bold */
    s.nextSlice("\x1b[0;3mB");  /* Italic */
    s.nextSlice("\x1b[0;4mC");  /* Underline */

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Verify we have the right number of rows */
    RenderState::Row *row_data = state.row_data.items;
    ASSERT_TRUE(3 == state.row_data.items_len);

    /* All rows should have cols cells */
    ASSERT_TRUE(10 == row_data[0].cells.items_len);
    ASSERT_TRUE(10 == row_data[1].cells.items_len);
    ASSERT_TRUE(10 == row_data[2].cells.items_len);

    /* Row zero should contain our text */
    {
        const RenderState::Cell cell = row_data[0].cells.items[0];
        ASSERT_TRUE('A' == cell.raw.codepoint());
        ASSERT_TRUE(cell.style.flags.bold);
    }
    {
        const RenderState::Cell cell = row_data[0].cells.items[1];
        ASSERT_TRUE('B' == cell.raw.codepoint());
        ASSERT_TRUE(!cell.style.flags.bold);
        ASSERT_TRUE(cell.style.flags.italic);
    }
    ASSERT_TRUE('C' == row_data[0].cells.items[2].raw.codepoint());
    ASSERT_TRUE(0 == row_data[0].cells.items[3].raw.codepoint());
}

/* Verifies that an incrementally updated render state has identical
 * contents to a from-scratch rebuild. This is the load-bearing check
 * for our dirty tracking: if any terminal operation changes row
 * contents without setting a dirty signal that `update` honors
 * (terminal dirty, screen dirty, page dirty, row dirty, viewport pin,
 * or dimensions), the incremental state will contain stale rows and
 * this comparison will fail. */
static void testCompareStates(const RenderState *incremental, const RenderState *fresh) {
    /* Row metadata that is allowed to be stale in an incremental
     * update. Dirty tracking only guarantees that VISUAL changes are
     * flagged (see page.Row.dirty); these fields are non-visual
     * metadata that the terminal may change without dirtying the row
     * (e.g. Screen.cursorResetWrap clears wrap flags without a dirty
     * mark). This staleness predates the chunked update
     * implementation; it is present in the row-iterator implementation
     * as well. */
    const page::Mask<page::Row, 1> StaleOkMask(page::fields::row_wrap() | page::fields::row_wrap_continuation() |
                                               page::fields::row_semantic_prompt() | page::fields::row_dirty());

    ASSERT_TRUE(fresh->rows == incremental->rows);
    ASSERT_TRUE(fresh->cols == incremental->cols);
    ASSERT_TRUE(fresh->cursor.active.eql(incremental->cursor.active));
    ASSERT_TRUE(fresh->cursor.viewport.has == incremental->cursor.viewport.has);
    if (fresh->cursor.viewport.has) {
        ASSERT_TRUE(fresh->cursor.viewport.value.x == incremental->cursor.viewport.value.x);
        ASSERT_TRUE(fresh->cursor.viewport.value.y == incremental->cursor.viewport.value.y);
        ASSERT_TRUE(fresh->cursor.viewport.value.wide_tail == incremental->cursor.viewport.value.wide_tail);
    }
    ASSERT_TRUE((page::Cell::Backing)fresh->cursor.cell.bits ==
                (page::Cell::Backing)incremental->cursor.cell.bits);

    const RenderState::Row *inc_data = incremental->row_data.items;
    const RenderState::Row *new_data = fresh->row_data.items;
    ASSERT_TRUE(fresh->row_data.items_len == incremental->row_data.items_len);
    const size_t new_data_len = fresh->row_data.items_len;
    for (size_t y = 0; y < new_data_len; y++) {
        /* Wisp: `errdefer std.log.warn("mismatch on row y={}", ...)` — the
         * port has no logging; the assertion already prints the line. */

        /* Pins must match exactly. */
        const Pin inc_pin = inc_data[y].pin;
        const Pin new_pin = new_data[y].pin;
        ASSERT_TRUE(new_pin.node == inc_pin.node);
        ASSERT_TRUE(new_pin.y == inc_pin.y);

        /* Raw row data must match, except for non-visual metadata
         * fields which may legitimately be stale (see StaleOkMask). */
        const page::Row inc_row = inc_data[y].raw;
        const page::Row new_row = new_data[y].raw;
        ASSERT_TRUE(StaleOkMask.strip(new_row) == StaleOkMask.strip(inc_row));

        const RenderState::Cell *inc_cells = inc_data[y].cells.items;
        const RenderState::Cell *new_cells = new_data[y].cells.items;
        ASSERT_TRUE(new_data[y].cells.items_len == inc_data[y].cells.items_len);
        const size_t new_cells_len = new_data[y].cells.items_len;
        const bool managed = new_row.managedMemory();
        for (size_t x = 0; x < new_cells_len; x++) {
            /* Raw cell contents must match. */
            const page::Cell inc_cell = inc_cells[x].raw;
            const page::Cell new_cell = new_cells[x].raw;
            ASSERT_TRUE((page::Cell::Backing)new_cell.bits == (page::Cell::Backing)inc_cell.bits);

            /* The style is only defined if the cell is styled or is
             * a bg-color cell within a row that has managed memory. */
            bool bg_cell = false;
            switch (new_cell.content_tag()) {
            case page::Cell::ContentTag::bg_color_rgb:
            case page::Cell::ContentTag::bg_color_palette: bg_cell = true; break;
            default: bg_cell = false; break;
            }
            if (new_cell.style_id() != 0 || (managed && bg_cell)) {
                ASSERT_TRUE(new_cells[x].style.eql(inc_cells[x].style));
            }

            /* Graphemes are only defined for grapheme cells. */
            if (new_cell.content_tag() == page::Cell::ContentTag::codepoint_grapheme) {
                ASSERT_TRUE(new_cells[x].grapheme_len == inc_cells[x].grapheme_len);
                for (size_t gi = 0; gi < new_cells[x].grapheme_len; gi++) {
                    ASSERT_TRUE(new_cells[x].grapheme[gi] == inc_cells[x].grapheme[gi]);
                }
            }
        }
    }
}

TEST(render, incremental_updates_match_full_rebuild) {
    /* Deterministic so failures are reproducible. */
    DefaultPrng prng = DefaultPrng::init(0xB0BACAFEull);
    Random rand(&prng);

    Terminal::Options t_opts((size::CellCountInt)20, (size::CellCountInt)8);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)500);
    TermHolder t_holder(t_opts);
    ASSERT_TRUE(t_holder.ok);
    Terminal &t = t_holder.t;

    STREAM(s, t);

    StateHolder inc_holder;
    RenderState &inc = inc_holder.s;

    char buf[64];
    for (size_t iter = 0; iter < 300; iter++) {
        /* Perform a random batch of operations between updates. */
        const size_t batch = rand.intRangeAtMost<size_t>(1, 6);
        for (size_t bi = 0; bi < batch; bi++) {
            switch (rand.intRangeAtMost<uint8_t>(0, 18)) {
            /* Plain text (possibly wrapping and scrolling). */
            case 0:
            case 1:
            case 2: {
                const size_t n = rand.intRangeAtMost<size_t>(1, 30);
                for (size_t k = 0; k < n; k++) {
                    const char c = (char)rand.intRangeAtMost<uint8_t>('A', 'Z');
                    s.nextSlice(&c, 1);
                }
                break;
            }

            /* Newlines to build scrollback and trigger pruning. */
            case 3:
            case 4: {
                const size_t n = rand.intRangeAtMost<size_t>(1, 10);
                for (size_t k = 0; k < n; k++) s.nextSlice("x\r\n");
                break;
            }

            /* Cursor movement. */
            case 5:
                snprintf(buf, sizeof(buf), "\x1b[%u;%uH", (unsigned)rand.intRangeAtMost<uint16_t>(1, 8),
                         (unsigned)rand.intRangeAtMost<uint16_t>(1, 20));
                s.nextSlice(buf);
                break;

            /* Styling: bold, truecolor bg, palette fg, reset. */
            case 6:
                switch (rand.intRangeAtMost<uint8_t>(0, 3)) {
                case 0: s.nextSlice("\x1b[1m"); break;
                case 1: s.nextSlice("\x1b[48;2;30;60;90m"); break;
                case 2: s.nextSlice("\x1b[38;5;120m"); break;
                default: s.nextSlice("\x1b[0m"); break;
                }
                break;

            /* Erase ops (EL, ED variants including scrollback). */
            case 7:
                switch (rand.intRangeAtMost<uint8_t>(0, 4)) {
                case 0: s.nextSlice("\x1b[K"); break;
                case 1: s.nextSlice("\x1b[1K"); break;
                case 2: s.nextSlice("\x1b[J"); break;
                case 3: s.nextSlice("\x1b[2J"); break;
                default: s.nextSlice("\x1b[3J"); break;
                }
                break;

            /* Insert/delete lines (row rotations within regions). */
            case 8:
                snprintf(buf, sizeof(buf), "\x1b[%uL", (unsigned)rand.intRangeAtMost<uint16_t>(1, 4));
                s.nextSlice(buf);
                break;
            case 9:
                snprintf(buf, sizeof(buf), "\x1b[%uM", (unsigned)rand.intRangeAtMost<uint16_t>(1, 4));
                s.nextSlice(buf);
                break;

            /* Scroll up/down (page-dirty row rotations). */
            case 10:
                snprintf(buf, sizeof(buf), "\x1b[%uS", (unsigned)rand.intRangeAtMost<uint16_t>(1, 4));
                s.nextSlice(buf);
                break;
            case 11:
                snprintf(buf, sizeof(buf), "\x1b[%uT", (unsigned)rand.intRangeAtMost<uint16_t>(1, 4));
                s.nextSlice(buf);
                break;

            /* Set/reset scroll regions to exercise bounded scrolls. */
            case 12: {
                const uint16_t top = rand.intRangeAtMost<uint16_t>(1, 4);
                const uint16_t bot = rand.intRangeAtMost<uint16_t>((uint16_t)(top + 1), 8);
                snprintf(buf, sizeof(buf), "\x1b[%u;%ur", (unsigned)top, (unsigned)bot);
                s.nextSlice(buf);
                break;
            }

            /* Insert/delete/erase chars within a row. */
            case 13:
                snprintf(buf, sizeof(buf), "\x1b[%u@", (unsigned)rand.intRangeAtMost<uint16_t>(1, 5));
                s.nextSlice(buf);
                break;
            case 14:
                snprintf(buf, sizeof(buf), "\x1b[%uP", (unsigned)rand.intRangeAtMost<uint16_t>(1, 5));
                s.nextSlice(buf);
                break;

            /* Reverse index (scroll down at top). */
            case 15: s.nextSlice("\x1bM"); break;

            /* Wide chars and multi-codepoint graphemes. */
            case 16: s.nextSlice("\xe5\xad\x97\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"); break;

            /* Alternate screen switching (screen key redraw path). */
            case 17: s.nextSlice(rand.boolean() ? "\x1b[?1049h" : "\x1b[?1049l"); break;

            /* DECALN full-screen fill. */
            case 18: s.nextSlice("\x1b#8"); break;

            default: break; /* Wisp: `else => unreachable` */
            }
        }

        /* Occasionally scroll the viewport into scrollback and back. */
        switch (rand.intRangeAtMost<uint8_t>(0, 9)) {
        case 0: t.scrollViewport(Terminal::ScrollViewport::makeDelta(-3)); break;
        case 1: t.scrollViewport(Terminal::ScrollViewport::makeDelta(2)); break;
        case 2: t.scrollViewport(Terminal::ScrollViewport::makeBottom()); break;
        case 3: t.scrollViewport(Terminal::ScrollViewport::makeTop()); break;
        default: break;
        }

        /* Update our incremental state first: it must consume the dirty
         * state. The fresh state always fully rebuilds (its dimensions
         * start empty so it always redraws) and so does not depend on
         * any dirty flags. */
        ASSERT_TRUE(inc.update(talloc(), &t));

        StateHolder fresh_holder;
        RenderState &fresh = fresh_holder.s;
        ASSERT_TRUE(fresh.update(talloc(), &t));

        testCompareStates(&inc, &fresh);
        if (g_fail_count > 0) return;
    }
}

TEST(render, begin_and_end_update) {
    TERM(t, 10, 3);

    STREAM(s, t);
    s.nextSlice("\x1b[1mAB");  /* Bold */
    s.nextSlice("\x1b[0;3mC"); /* Italic */

    STATE(state);
    ASSERT_TRUE(state.beginUpdate(talloc(), &t));

    /* We should have pending style runs on row 0: one for the bold
     * run and one for the italic run. */
    {
        const RenderState::StyleRun *runs = state.pending_styles.items;
        ASSERT_TRUE(2 == state.pending_styles.items_len);
        ASSERT_TRUE(0 == runs[0].y);
        ASSERT_TRUE(0 == runs[0].start);
        ASSERT_TRUE(2 == runs[0].end);
        ASSERT_TRUE(runs[0].style.flags.bold);
        ASSERT_TRUE(0 == runs[1].y);
        ASSERT_TRUE(2 == runs[1].start);
        ASSERT_TRUE(3 == runs[1].end);
        ASSERT_TRUE(runs[1].style.flags.italic);
    }

    /* End our update. This should denormalize the runs into cells
     * and clear the pending runs. */
    state.endUpdate();
    {
        ASSERT_TRUE(0 == state.pending_styles.items_len);

        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(row_data[0].cells.items[0].style.flags.bold);
        ASSERT_TRUE(row_data[0].cells.items[1].style.flags.bold);
        ASSERT_TRUE(row_data[0].cells.items[2].style.flags.italic);
    }
}

TEST(render, endUpdate_skips_unchanged_style_runs) {
    TERM(t, 10, 3);

    STREAM(s, t);
    s.nextSlice("\x1b[1mAB"); /* Bold */

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    /* The applied cache should record the bold run for row 0. */
    {
        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(1 == row_data[0].applied_styles.items_len);
        ASSERT_TRUE(row_data[0].applied_styles.items[0].style.flags.bold);
        ASSERT_TRUE(state.row_data.items[0].cells.items[0].style.flags.bold);
    }

    /* Rewrite the text without changing the styling: the row is
     * rebuilt, the run matches the cache, and the styles must remain
     * correct (the fill is skipped internally). */
    s.nextSlice("\x1b[1;1H\x1b[1mXY");
    ASSERT_TRUE(state.update(talloc(), &t));
    {
        zigstd::ArrayListUnmanaged<RenderState::Cell> *cells = &state.row_data.items[0].cells;
        ASSERT_TRUE('X' == cells->items[0].raw.codepoint());
        ASSERT_TRUE(cells->items[0].style.flags.bold);
        ASSERT_TRUE(cells->items[1].style.flags.bold);
    }

    /* Change the styling: the cache mismatches and the new styles
     * must be applied and recorded. */
    s.nextSlice("\x1b[1;1H\x1b[0;3mZW"); /* Italic */
    ASSERT_TRUE(state.update(talloc(), &t));
    {
        zigstd::ArrayListUnmanaged<RenderState::Cell> *cells = &state.row_data.items[0].cells;
        ASSERT_TRUE('Z' == cells->items[0].raw.codepoint());
        ASSERT_TRUE(!cells->items[0].style.flags.bold);
        ASSERT_TRUE(cells->items[0].style.flags.italic);

        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(1 == row_data[0].applied_styles.items_len);
        ASSERT_TRUE(row_data[0].applied_styles.items[0].style.flags.italic);
    }
}

TEST(render, bg_color_cells) {
    TERM(t, 10, 3);

    STREAM(s, t);

    /* Write a styled cell (so the row has managed memory) then erase
     * the rest of the line with a palette background pending. The
     * erase produces bg_color content cells rather than styled cells. */
    s.nextSlice("\x1b[1mA\x1b[48;5;1m\x1b[K");

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    RenderState::Row *row_data = state.row_data.items;
    {
        const RenderState::Cell cell = row_data[0].cells.items[0];
        ASSERT_TRUE('A' == cell.raw.codepoint());
        ASSERT_TRUE(cell.style.flags.bold);
    }
    for (size_t x = 1; x < 10; x++) {
        const RenderState::Cell cell = row_data[0].cells.items[x];
        ASSERT_TRUE(page::Cell::ContentTag::bg_color_palette == cell.raw.content_tag());
        ASSERT_TRUE(Style::Color::makePalette(1).eql(cell.style.bg_color));
    }
}

TEST(render, grapheme) {
    TERM(t, 10, 3);

    STREAM(s, t);
    s.nextSlice("A");
    s.nextSlice("\xf0\x9f\x91\xa8\xe2\x80\x8d"); /* this has a ZWJ */

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Verify we have the right number of rows */
    RenderState::Row *row_data = state.row_data.items;
    ASSERT_TRUE(3 == state.row_data.items_len);

    /* All rows should have cols cells */
    ASSERT_TRUE(10 == row_data[0].cells.items_len);
    ASSERT_TRUE(10 == row_data[1].cells.items_len);
    ASSERT_TRUE(10 == row_data[2].cells.items_len);

    /* Row zero should contain our text */
    {
        const RenderState::Cell cell = row_data[0].cells.items[0];
        ASSERT_TRUE('A' == cell.raw.codepoint());
    }
    {
        const RenderState::Cell cell = row_data[0].cells.items[1];
        ASSERT_TRUE(0x1F468 == cell.raw.codepoint());
        ASSERT_TRUE(page::Cell::Wide::wide == cell.raw.wide());
        ASSERT_TRUE(1 == cell.grapheme_len);
        ASSERT_TRUE(0x200D == cell.grapheme[0]);
    }
    {
        const RenderState::Cell cell = row_data[0].cells.items[2];
        ASSERT_TRUE(0 == cell.raw.codepoint());
        ASSERT_TRUE(page::Cell::Wide::spacer_tail == cell.raw.wide());
    }
}

TEST(render, cursor_state_in_viewport) {
    TERM(t, 10, 5);

    STREAM(s, t);
    s.nextSlice("A\x1b[H");

    STATE(state);

    /* Initial update */
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(0 == state.cursor.active.x);
    ASSERT_TRUE(0 == state.cursor.active.y);
    ASSERT_TRUE(state.cursor.viewport.has);
    ASSERT_TRUE(0 == state.cursor.viewport.value.x);
    ASSERT_TRUE(0 == state.cursor.viewport.value.y);
    ASSERT_TRUE('A' == state.cursor.cell.codepoint());
    ASSERT_TRUE(state.cursor.style.default_());

    /* Set a style on the cursor */
    s.nextSlice("\x1b[1m"); /* Bold */
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(!state.cursor.style.default_());
    ASSERT_TRUE(state.cursor.style.flags.bold);
    s.nextSlice("\x1b[0m"); /* Reset style */

    /* Move cursor to 2,1 */
    s.nextSlice("\x1b[2;3H");
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(2 == state.cursor.active.x);
    ASSERT_TRUE(1 == state.cursor.active.y);
    ASSERT_TRUE(state.cursor.viewport.has);
    ASSERT_TRUE(2 == state.cursor.viewport.value.x);
    ASSERT_TRUE(1 == state.cursor.viewport.value.y);
}

TEST(render, cursor_state_out_of_viewport) {
    TERM(t, 10, 2);

    STREAM(s, t);
    s.nextSlice("A\r\nB\r\nC\r\nD\r\n");

    STATE(state);

    /* Initial update */
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(0 == state.cursor.active.x);
    ASSERT_TRUE(1 == state.cursor.active.y);
    ASSERT_TRUE(state.cursor.viewport.has);
    ASSERT_TRUE(0 == state.cursor.viewport.value.x);
    ASSERT_TRUE(1 == state.cursor.viewport.value.y);

    /* Scroll the viewport */
    t.scrollViewport(Terminal::ScrollViewport::makeTop());
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Set a style on the cursor */
    ASSERT_TRUE(0 == state.cursor.active.x);
    ASSERT_TRUE(1 == state.cursor.active.y);
    ASSERT_TRUE(!state.cursor.viewport.has);
}

TEST(render, dirty_state) {
    TERM(t, 10, 5);

    STREAM(s, t);

    STATE(state);

    /* First update should trigger redraw due to resize */
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(RenderState::Dirty::full == state.dirty);

    /* Reset dirty flag and dirty rows */
    state.dirty = RenderState::Dirty::false_;
    {
        RenderState::Row *row_data = state.row_data.items;
        for (size_t i = 0; i < state.row_data.items_len; i++) row_data[i].dirty = false;
    }

    /* Second update with no changes - no dirty rows */
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(RenderState::Dirty::false_ == state.dirty);
    {
        RenderState::Row *row_data = state.row_data.items;
        for (size_t i = 0; i < state.row_data.items_len; i++) ASSERT_TRUE(!row_data[i].dirty);
    }

    /* Write to first line */
    s.nextSlice("A");
    ASSERT_TRUE(state.update(talloc(), &t));
    ASSERT_TRUE(RenderState::Dirty::partial == state.dirty);
    {
        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(row_data[0].dirty);  /* First row dirty */
        ASSERT_TRUE(!row_data[1].dirty); /* Second row clean */
    }
}

TEST(render, colors) {
    TERM(t, 10, 5);

    STREAM(s, t);

    STATE(state);

    /* Default colors */
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Change cursor color */
    s.nextSlice("\x1b]12;#FF0000\x07");
    ASSERT_TRUE(state.update(talloc(), &t));

    ASSERT_TRUE(state.colors.cursor.has);
    const terminal::RGB c = state.colors.cursor.value;
    ASSERT_TRUE(0xFF == c.r);
    ASSERT_TRUE(0 == c.g);
    ASSERT_TRUE(0 == c.b);

    /* Change palette color 0 to White */
    s.nextSlice("\x1b]4;0;#FFFFFF\x07");
    ASSERT_TRUE(state.update(talloc(), &t));
    const terminal::RGB p0 = state.colors.palette[0];
    ASSERT_TRUE(0xFF == p0.r);
    ASSERT_TRUE(0xFF == p0.g);
    ASSERT_TRUE(0xFF == p0.b);
}

TEST(render, selection_single_line) {
    TERM(t, 10, 3);

    Screen *screen = t.screens.active;
    {
        const Maybe<Pin> a = screen->pages.pin(point::Point::active(0, 1));
        const Maybe<Pin> b = screen->pages.pin(point::Point::active(2, 1));
        ASSERT_TRUE(a.has);
        ASSERT_TRUE(b.has);
        ASSERT_TRUE(screen->select(Maybe<Selection>(Selection::init(a.value, b.value, false))));
    }

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    RenderState::Row *row_data = state.row_data.items;
    ASSERT_TRUE(!row_data[0].selection.has);
    ASSERT_TRUE(row_data[1].selection.has);
    ASSERT_TRUE(0 == row_data[1].selection.value.start);
    ASSERT_TRUE(2 == row_data[1].selection.value.end);
    ASSERT_TRUE(!row_data[2].selection.has);

    /* Clear the selection */
    ASSERT_TRUE(screen->select(Maybe<Selection>::none()));
    ASSERT_TRUE(state.update(talloc(), &t));
    row_data = state.row_data.items;
    ASSERT_TRUE(!row_data[0].selection.has);
    ASSERT_TRUE(!row_data[1].selection.has);
    ASSERT_TRUE(!row_data[2].selection.has);
}

TEST(render, selection_multiple_lines) {
    TERM(t, 10, 3);

    Screen *screen = t.screens.active;
    {
        const Maybe<Pin> a = screen->pages.pin(point::Point::active(0, 1));
        const Maybe<Pin> b = screen->pages.pin(point::Point::active(2, 2));
        ASSERT_TRUE(a.has);
        ASSERT_TRUE(b.has);
        ASSERT_TRUE(screen->select(Maybe<Selection>(Selection::init(a.value, b.value, false))));
    }

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    RenderState::Row *row_data = state.row_data.items;
    ASSERT_TRUE(!row_data[0].selection.has);
    ASSERT_TRUE(row_data[1].selection.has);
    ASSERT_TRUE(0 == row_data[1].selection.value.start);
    ASSERT_TRUE((size::CellCountInt)(screen->pages.cols - 1) == row_data[1].selection.value.end);
    ASSERT_TRUE(row_data[2].selection.has);
    ASSERT_TRUE(0 == row_data[2].selection.value.start);
    ASSERT_TRUE(2 == row_data[2].selection.value.end);
}

TEST(render, linkCells) {
    TERM(t, 10, 5);

    STREAM(s, t);

    STATE(state);

    /* Create a hyperlink */
    s.nextSlice("\x1b]8;;http://example.com\x1b\\LINK\x1b]8;;\x1b\\");
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Query link at 0,0 */
    RenderState::CellSet cells;
    ASSERT_TRUE(state.linkCells(talloc(), point::Coordinate(0, 0), &cells));

    ASSERT_TRUE(4 == cells.count());
    ASSERT_TRUE(cells.contains(point::Coordinate(0, 0)));
    ASSERT_TRUE(cells.contains(point::Coordinate(1, 0)));
    ASSERT_TRUE(cells.contains(point::Coordinate(2, 0)));
    ASSERT_TRUE(cells.contains(point::Coordinate(3, 0)));
    cells.deinit(talloc());

    /* Query no link */
    RenderState::CellSet cells2;
    ASSERT_TRUE(state.linkCells(talloc(), point::Coordinate(4, 0), &cells2));
    ASSERT_TRUE(0 == cells2.count());
    cells2.deinit(talloc());
}

TEST(render, string) {
    TERM(t, 5, 2);

    STREAM(s, t);
    s.nextSlice("AB");

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    std::string w;

    ASSERT_TRUE(state.string(&w, nullptr, talloc()));

    static const char expected_bytes[] = "AB\x00\x00\x00\n\x00\x00\x00\x00\x00\n";
    const size_t expected_len = sizeof(expected_bytes) - 1;
    ASSERT_TRUE(expected_len == w.size());
    ASSERT_TRUE(memcmp(expected_bytes, w.data(), expected_len) == 0);
}

TEST(render, linkCells_with_scrollback_spanning_pages) {
    const size::CellCountInt viewport_rows = 10;
    const size::CellCountInt tail_rows = 5;

    Terminal::Options t_opts(page::std_capacity().cols, viewport_rows);
    t_opts.max_scrollback_bytes = Maybe<size_t>((size_t)10000);
    TermHolder t_holder(t_opts);
    ASSERT_TRUE(t_holder.ok);
    Terminal &t = t_holder.t;

    STREAM(s, t);

    PageList *pages = &t.screens.active->pages;
    const size::CellCountInt first_page_cap = pages->pages.first->capacity().rows;

    /* Fill first page.
     * Wisp: Zig evaluates `0..first_page_cap - 1` once; the bound is a local
     * snapshot so the loop trip count cannot follow the growing page list. */
    {
        const size_t n = (size_t)first_page_cap - 1;
        for (size_t i = 0; i < n; i++) s.nextSlice("\r\n");
    }

    /* Create second page with hyperlink */
    s.nextSlice("\r\n");
    s.nextSlice("\x1b]8;;http://example.com\x1b\\LINK\x1b]8;;\x1b\\");
    {
        const size_t n = (size_t)(tail_rows - 1);
        for (size_t i = 0; i < n; i++) s.nextSlice("\r\n");
    }

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    const size_t expected_viewport_y = (size_t)(viewport_rows - tail_rows);
    /* BUG: This crashes without the fix */
    RenderState::CellSet cells;
    ASSERT_TRUE(state.linkCells(talloc(), point::Coordinate(0, (uint32_t)expected_viewport_y), &cells));
    const size_t count = cells.count();
    cells.deinit(talloc());
    ASSERT_TRUE((size_t)4 == count);
}

TEST(render, linkCells_with_invalid_viewport_point) {
    TERM(t, 10, 5);

    STREAM(s, t);
    (void)s;

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Row out of bound */
    {
        RenderState::CellSet cells;
        ASSERT_TRUE(state.linkCells(talloc(), point::Coordinate(0, (uint32_t)(t.rows + 10)), &cells));
        const size_t count = cells.count();
        cells.deinit(talloc());
        ASSERT_TRUE(0 == count);
    }

    /* Col out of bound */
    {
        RenderState::CellSet cells;
        ASSERT_TRUE(state.linkCells(talloc(), point::Coordinate((size::CellCountInt)(t.cols + 10), 0), &cells));
        const size_t count = cells.count();
        cells.deinit(talloc());
        ASSERT_TRUE(0 == count);
    }
}

TEST(render, flattened_highlights_require_matching_page_serial) {
    TERM(t, 10, 3);

    /* Capture the live generation while terminal-owned state is in scope so
     * we can also verify beginUpdate copies it into the render row. */
    const Pin live_pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);
    const uint64_t live_serial = live_pin.node->serial;

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    Pin pin;
    {
        RenderState::Row *row_data = state.row_data.items;
        for (size_t i = 0; i < state.row_data.items_len; i++) row_data[i].dirty = false;
        state.dirty = RenderState::Dirty::false_;
        pin = row_data[0].pin;
    }
    const uint64_t row_serial = state.row_data.items[0].serial;
    ASSERT_TRUE(live_pin.node == pin.node);
    ASSERT_TRUE(live_serial == row_serial);

    /* Use the exact node pointer and row captured by the render state, but a
     * different generation. A reused node address must not make this stale
     * flattened highlight match. */
    highlight::Flattened hl = highlight::Flattened::empty();
    hl.top_x = 2;
    hl.bot_x = 4;
    {
        highlight::Flattened::Chunk c;
        c.node = pin.node;
        c.serial = live_serial ^ 1;
        c.start = pin.y;
        c.end = (size::CellCountInt)(pin.y + 1);
        hl.chunks.push_back(c);
    }

    ASSERT_TRUE(state.updateHighlightsFlattened(talloc(), 42, &hl, 1));
    {
        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(0 == row_data[0].highlights.items_len);
        ASSERT_TRUE(!row_data[0].dirty);
        ASSERT_TRUE(RenderState::Dirty::false_ == state.dirty);
    }

    /* The same chunk is accepted once its copied serial also matches. */
    hl.chunks[0].serial = live_serial;
    ASSERT_TRUE(state.updateHighlightsFlattened(talloc(), 42, &hl, 1));
    {
        RenderState::Row *row_data = state.row_data.items;
        const RenderState::Highlight *row_highlights = row_data[0].highlights.items;
        ASSERT_TRUE(1 == row_data[0].highlights.items_len);
        ASSERT_TRUE(42 == row_highlights[0].tag);
        ASSERT_TRUE(2 == row_highlights[0].range[0]);
        ASSERT_TRUE(4 == row_highlights[0].range[1]);
        ASSERT_TRUE(row_data[0].dirty);
        ASSERT_TRUE(RenderState::Dirty::partial == state.dirty);
    }

    hl.deinit();
}

TEST(render, dirty_row_resets_highlights) {
    TERM(t, 10, 3);

    STREAM(s, t);
    s.nextSlice("ABC");

    STATE(state);
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Reset dirty state */
    state.dirty = RenderState::Dirty::false_;
    {
        RenderState::Row *row_data = state.row_data.items;
        for (size_t i = 0; i < state.row_data.items_len; i++) row_data[i].dirty = false;
    }

    /* Manually add a highlight to row 0 */
    {
        RenderState::Row *row_data = state.row_data.items;
        zigstd::ArenaAllocator arena = row_data[0].arena.promote(talloc());
        RenderState::Highlight h;
        h.tag = 1;
        h.range[0] = 0;
        h.range[1] = 2;
        const bool ok = row_data[0].highlights.append(arena.allocator(), h);
        row_data[0].arena = arena.state();
        ASSERT_TRUE(ok);
    }

    /* Verify we have a highlight */
    {
        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(1 == row_data[0].highlights.items_len);
    }

    /* Write to row 0 to make it dirty */
    s.nextSlice("\x1b[H"); /* Move to home */
    s.nextSlice("X");
    ASSERT_TRUE(state.update(talloc(), &t));

    /* Verify the highlight was reset on the dirty row */
    {
        RenderState::Row *row_data = state.row_data.items;
        ASSERT_TRUE(0 == row_data[0].highlights.items_len);
    }
}

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(render, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
