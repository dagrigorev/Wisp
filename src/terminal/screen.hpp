/* Reimplemented after Ghostty src/terminal/Screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * REIMPLEMENTED, NOT TRANSLITERATED. This file was written from Ghostty's
 * design and verified against Wisp's own tests, without the upstream source
 * to hand. It follows upstream's structure but has not been checked against
 * it line by line, and its behaviour will differ in places. It is due to be
 * replaced by a transliteration checked against upstream's own tests, as
 * parser.hpp has been.
 *
 * A screen: the pages, and the cursor moving over them.
 *
 * PageList holds the text and knows nothing about where anyone is looking at
 * it. A Screen adds the cursor — where the next character goes — and the
 * operations that move it. That sounds small, and the struct is small, but it
 * is where two things that have been kept apart so far finally meet: a
 * position on the screen, which is a column and a row counted from the top of
 * what is displayed, and a position in the text, which is a pin.
 *
 * The cursor is both, and keeping them agreeing is most of this file. The
 * screen coordinates are what a terminal program means by the cursor and what
 * every escape sequence is written in terms of. The pin is what survives the
 * pages changing underneath it. Neither can be dropped: coordinates alone
 * would be wrong after a reflow, and a pin alone cannot answer "what column
 * am I in" without a walk.
 *
 * NOT THE LIVE SCREEN. src/terminal/screen.h is Wisp's working terminal
 * buffer, which is what the application currently runs on. This is the ported
 * one, built on the ported page list, and the two are unrelated for now.
 *
 * PARTIAL PORT. The screen, the cursor, its movement, writing text, erasing,
 * selections and the saved cursor are here. Upstream's remaining pieces are
 * not, so the ledger records Screen.zig as `wip`.
 *
 * The alternate screen is not one of them: upstream a terminal owns two
 * screens and swaps between them, so it arrives with Terminal rather than
 * here.
 */

#pragma once
#ifndef WISP_TERMINAL_SCREEN_HPP
#define WISP_TERMINAL_SCREEN_HPP

#include <stddef.h>
#include <stdint.h>

#include "page_list.hpp"

namespace wisp {
namespace terminal {

/* ─── the cursor ─────────────────────────────────────────────────────────── */

/* Where the next character goes.
 *
 * x and y are screen coordinates: columns from the left, rows from the top of
 * the active area. They are what escape sequences talk about, and they are
 * only meaningful against the screen as it is now.
 *
 * page_pin is the same place as a pin, tracked so the list maintains it. The
 * two are kept in step by every operation here, and page_row and page_cell
 * are cached from the pin so that writing a character does not cost a lookup.
 *
 * Those cached pointers are the dangerous part. They are addresses inside a
 * page, and every operation that can move rows or replace a page invalidates
 * them. Anything that touches the list must reload them afterwards, which is
 * what screen_cursor_reload is for. */
struct Cursor {
    CellCountInt x;
    CellCountInt y;

    TrackedPin page_pin;

    Row  *page_row;
    Cell *page_cell;

    /* The style new characters are written with. The value is kept rather
     * than an interned ID because an ID belongs to one page's style set, and
     * the cursor crosses pages. Interning happens when a character is
     * actually written. */
    style::Style style;

    /* The cursor is at the right margin and the next character wraps rather
     * than overwriting. DECAWM's doing; kept here because it is a property of
     * where the cursor is, not of the text. */
    bool pending_wrap;

    /* The OSC 8 link new characters are written into, if one is open.
     *
     * Like the style, this is the cursor's rather than anything on the
     * screen's: it is a property of what is written next. And like the style
     * it is held as a value rather than an interned ID, because the ID
     * belongs to one page and the cursor crosses pages. The strings are
     * copied in, since the parser's buffer they arrive in is reused by the
     * very next sequence. */
    bool     hyperlink_active;
    char     hyperlink_uri[512];
    size_t   hyperlink_uri_len;
    char     hyperlink_id[128];
    size_t   hyperlink_id_len;
    uint32_t hyperlink_implicit;

    Cursor()
        : x(0), y(0), page_pin(), page_row(nullptr), page_cell(nullptr),
          style(), pending_wrap(false), hyperlink_active(false),
          hyperlink_uri_len(0), hyperlink_id_len(0), hyperlink_implicit(0) {
        hyperlink_uri[0] = '\0';
        hyperlink_id[0] = '\0';
    }
};

/* ─── selections ─────────────────────────────────────────────────────────── */

/* A selected region of the text.
 *
 * Both ends are tracked pins, and they have to be: a selection is made by a
 * user and then sits there while output arrives, the screen scrolls and the
 * window is resized. Screen coordinates would be wrong within a line of
 * output; the pins keep pointing at the characters that were selected.
 *
 * start and end are where the drag began and where it ended, in that order,
 * which is not necessarily top-to-bottom — selecting upwards is ordinary.
 * Anything that needs them the other way round asks for them ordered. */
struct Selection {
    TrackedPin start_pin;
    TrackedPin end_pin;

    /* A block selection: the columns between the two ends on every row,
     * rather than everything from one end to the other. */
    bool rectangle;

    bool active;

    Selection() : start_pin(), end_pin(), rectangle(false), active(false) {}
};

/* ─── the saved cursor ───────────────────────────────────────────────────── */

/* What DECSC puts away and DECRC brings back.
 *
 * It is not just a position. A program that saves the cursor, prints
 * something in another colour and restores expects its colour back too, so
 * the style goes with it — and so does the pending wrap, which is as much a
 * part of where the cursor is as the column.
 *
 * Coordinates rather than a pin, deliberately. DECSC means "this place on the
 * screen", and a program that saves the cursor, scrolls, and restores expects
 * the cursor back where it was on the *screen*, not chasing the line that has
 * since moved up. That is the opposite of what the cursor itself wants, and
 * the difference is the whole reason a pin and a coordinate are both kept.
 *
 * Upstream this also carries the character sets, which are Terminal's state
 * rather than the screen's; terminal_save_cursor saves them beside this. */
struct SavedCursor {
    CellCountInt x;
    CellCountInt y;
    style::Style style;
    bool pending_wrap;

    /* Nothing has been saved yet. A restore then means "go to the top left",
     * which is what the standard says an unsaved DECRC does. */
    bool valid;

    SavedCursor() : x(0), y(0), style(), pending_wrap(false), valid(false) {}
};

/* ─── the screen ─────────────────────────────────────────────────────────── */

struct Screen {
    PageList pages;
    Cursor   cursor;

    /* DECAWM: whether a character at the right margin wraps to the next line
     * or overwrites the last column.
     *
     * Upstream this is one of Terminal's modes, not the screen's, and it
     * belongs there — it is set by an escape sequence and applies to a
     * terminal rather than to a buffer. It lives here until Terminal is
     * ported, because writing text has to do something at the margin and
     * silently picking one behaviour would be worse than naming it. */
    bool auto_wrap;

    /* What the user has selected, if anything. See the selections section
     * below. */
    struct Selection selection;

    /* Where DECSC put the cursor. */
    SavedCursor saved;

    Screen();
};

inline Screen::Screen()
    : pages(), cursor(), auto_wrap(true), selection(), saved() {}

/* Forget the selection, releasing the pins the list was maintaining. */
inline void screen_select_clear(Screen *s) {
    if (!s->selection.active) return;
    page_list_untrack(&s->pages, &s->selection.start_pin);
    page_list_untrack(&s->pages, &s->selection.end_pin);

    /* Emptied as well as untracked. Untracking on its own only stops them
     * being maintained, and a forgotten selection still holding the last
     * place it was is a pin naming a page that can now be freed with nobody
     * left to fix it up. */
    s->selection.start_pin.pin = Pin();
    s->selection.end_pin.pin = Pin();

    s->selection.active = false;
    s->selection.rectangle = false;
}

/* Point the cursor's cached row and cell at whatever its pin now names.
 *
 * Every operation that moves the cursor or disturbs the pages ends here. The
 * cached pointers are addresses inside a page, so a page replaced for a
 * bigger budget, a list rebuilt by a resize, or a row that scrolled away all
 * leave them pointing at memory that means something else — or nothing. */
inline void screen_cursor_reload(Screen *s) {
    Pin p = s->cursor.page_pin.pin;
    if (!p.valid()) {
        s->cursor.page_row = nullptr;
        s->cursor.page_cell = nullptr;
        return;
    }

    /* The pin carries its own column, and the cursor's x is the truth about
     * where it is, so the pin is brought into line rather than the reverse. */
    p.x = s->cursor.x;
    s->cursor.page_pin.pin.x = s->cursor.x;

    s->cursor.page_row = p.row();
    s->cursor.page_cell = p.cell();
}

/* Move the cursor to a row of the active area, by pin.
 *
 * y is a screen coordinate, so this is where the two ideas of position are
 * reconciled: the row is found by walking from the top of the active area,
 * and the pin that comes back is what will survive the next reflow. */
inline bool screen_cursor_absolute(Screen *s, CellCountInt x, CellCountInt y) {
    if (x >= s->pages.cols || y >= s->pages.rows) return false;

    Pin p = page_list_active_pin(&s->pages, x, y);
    if (!p.valid()) return false;

    s->cursor.x = x;
    s->cursor.y = y;
    s->cursor.page_pin.pin = p;
    s->cursor.pending_wrap = false;
    screen_cursor_reload(s);
    return true;
}

/* Build a screen and put the cursor at the top left. */
inline bool screen_init(Screen *s, CellCountInt cols, CellCountInt rows,
                        size_t max_scrollback_bytes) {
    *s = Screen();
    if (!page_list_init(&s->pages, cols, rows, max_scrollback_bytes)) {
        return false;
    }

    /* The cursor is tracked from the moment it exists. A cursor that was not
     * would be wrong after the first resize, which is not a state worth
     * having even briefly. */
    page_list_track(&s->pages, &s->cursor.page_pin,
                    page_list_active_start(&s->pages));

    return screen_cursor_absolute(s, 0, 0);
}

inline void screen_deinit(Screen *s) {
    screen_select_clear(s);
    page_list_untrack(&s->pages, &s->cursor.page_pin);
    page_list_deinit(&s->pages);
    s->cursor.page_row = nullptr;
    s->cursor.page_cell = nullptr;
}

/* ─── moving ─────────────────────────────────────────────────────────────── */

/* All of these clamp at the edges of the active area and none of them scroll.
 * Scrolling is a decision about the screen as a whole — whether the region
 * moves, whether a line joins the scrollback — and the caller that knows
 * about margins makes it. */

inline void screen_cursor_left(Screen *s, CellCountInt n) {
    const CellCountInt x = s->cursor.x > n ? (CellCountInt)(s->cursor.x - n) : 0;
    screen_cursor_absolute(s, x, s->cursor.y);
}

inline void screen_cursor_right(Screen *s, CellCountInt n) {
    const size_t want = (size_t)s->cursor.x + n;
    const size_t last = (size_t)(s->pages.cols - 1);
    screen_cursor_absolute(s, (CellCountInt)(want > last ? last : want),
                           s->cursor.y);
}

inline void screen_cursor_up(Screen *s, CellCountInt n) {
    const CellCountInt y = s->cursor.y > n ? (CellCountInt)(s->cursor.y - n) : 0;
    screen_cursor_absolute(s, s->cursor.x, y);
}

inline void screen_cursor_down(Screen *s, CellCountInt n) {
    const size_t want = (size_t)s->cursor.y + n;
    const size_t last = (size_t)(s->pages.rows - 1);
    screen_cursor_absolute(s, s->cursor.x,
                           (CellCountInt)(want > last ? last : want));
}

/* Move down a row, scrolling the screen if there is nowhere to move to.
 *
 * This is what a newline at the bottom does, and it is the only cursor
 * operation that changes the text rather than just where it is pointing. The
 * row that leaves the top of the screen is not moved anywhere: the active
 * area is the last rows of the list, so adding a row at the end is what makes
 * the old one scrollback. */
inline bool screen_cursor_down_scroll(Screen *s) {
    if (s->cursor.y + 1 < s->pages.rows) {
        screen_cursor_down(s, 1);
        return true;
    }

    if (!page_list_grow(&s->pages).valid()) return false;

    /* The cursor stays on the last row of the active area, which is now the
     * row that was just added. Its pin was maintained through the growth, but
     * the cached pointers were not. */
    return screen_cursor_absolute(s, s->cursor.x,
                                  (CellCountInt)(s->pages.rows - 1));
}


/* ─── writing ────────────────────────────────────────────────────────────── */

/* Give the cursor's cell the cursor's style, making room if there is none.
 *
 * The cursor carries a style value rather than an interned ID, because an ID
 * belongs to one page's style set and the cursor crosses pages. Interning
 * happens here, at the moment a character is actually written.
 *
 * A page can be out of style slots, and this is where the page budgets earn
 * their keep: the page is replaced by a roomier one holding the same rows.
 * That moves everything to a new address, so the cursor has to be reloaded
 * before the write is retried — a cached cell pointer from before the growth
 * points into a page that has been freed. */
inline bool screen_cursor_apply_style(Screen *s) {
    PageNode *node = s->cursor.page_pin.pin.node;
    if (!node) return false;

    const CellCountInt x = s->cursor.x;
    const CellCountInt y = s->cursor.page_pin.pin.y;

    if (node->page.set_cell_style(x, y, s->cursor.style)) return true;

    PageNode *fresh = page_list_grow_budget(&s->pages, node, PageBudget::styles);
    if (!fresh) return false;

    screen_cursor_reload(s);
    node = s->cursor.page_pin.pin.node;
    return node->page.set_cell_style(s->cursor.x, s->cursor.page_pin.pin.y,
                                     s->cursor.style);
}

/* Attach the cursor's open hyperlink to the cell under it, if there is one.
 *
 * A page runs out of room for links long before it runs out of rows, so this
 * is the other place the page budgets pay off. A link needs room in two
 * places — its strings in the string storage, and its entry in the link set,
 * which is sized from a budget of its own — and either can be what is full.
 * Both are doubled together, which costs one page rebuild rather than
 * guessing which was the problem and possibly needing a second.
 *
 * A link that still cannot be attached is dropped rather than failing the
 * write. The character is what the program sent; the link is decoration on
 * it, and losing the decoration is better than losing the text. */
inline bool screen_cursor_apply_hyperlink(Screen *s) {
    if (!s->cursor.hyperlink_active) return true;

    Pin p = s->cursor.page_pin.pin;
    if (!p.valid()) return true;

    const char *id = s->cursor.hyperlink_id_len ? s->cursor.hyperlink_id : nullptr;

    if (page_set_cell_hyperlink(&p.node->page, s->cursor.x, p.y,
                                s->cursor.hyperlink_uri,
                                s->cursor.hyperlink_uri_len, id,
                                s->cursor.hyperlink_id_len,
                                s->cursor.hyperlink_implicit)) {
        return true;
    }

    Capacity cap = p.node->page.capacity;
    const size_t strings = (size_t)cap.string_bytes * 2;
    const size_t links = (size_t)cap.hyperlink_bytes * 2;
    if (strings > (size_t)(StringBytesInt)-1 ||
        links > (size_t)(HyperlinkCountInt)-1) {
        return true;
    }
    cap.string_bytes = (StringBytesInt)strings;
    cap.hyperlink_bytes = (HyperlinkCountInt)links;

    if (!page_list_adjust_capacity(&s->pages, p.node, cap)) return true;

    screen_cursor_reload(s);
    p = s->cursor.page_pin.pin;
    page_set_cell_hyperlink(&p.node->page, s->cursor.x, p.y,
                            s->cursor.hyperlink_uri,
                            s->cursor.hyperlink_uri_len, id,
                            s->cursor.hyperlink_id_len,
                            s->cursor.hyperlink_implicit);
    return true;
}

/* Erase the cell about to be written, and the other half of any wide
 * character it belongs to.
 *
 * A wide character occupies two cells that only make sense together. Writing
 * over one of them without the other leaves a spacer with nothing to follow,
 * or a lead with nothing after it, and every later reader has to cope with a
 * state the terminal should never have produced. */
inline void screen_erase_for_write(Screen *s, CellCountInt x) {
    Pin p = s->cursor.page_pin.pin;
    if (!p.valid()) return;

    Page *page = &p.node->page;
    const CellCountInt y = p.y;

    Cell *c = page->get_cell(x, y);
    switch (c->wide()) {
        case Wide::wide:
            if ((size_t)x + 1 < (size_t)page->capacity.cols) {
                page_erase_cell(page, (CellCountInt)(x + 1), y);
            }
            break;
        case Wide::spacer_tail:
            if (x > 0) page_erase_cell(page, (CellCountInt)(x - 1), y);
            break;
        default:
            break;
    }

    page_erase_cell(page, x, y);
}

/* Move to the start of the next row, joining it to this one.
 *
 * The wrap flag is what makes the two rows one line, and it is the only
 * record that they were ever joined — without it a resize would treat the
 * break as one the program asked for. */
inline bool screen_wrap(Screen *s) {
    if (s->cursor.page_row) s->cursor.page_row->set_wrap(true);

    if (!screen_cursor_down_scroll(s)) return false;
    if (!screen_cursor_absolute(s, 0, s->cursor.y)) return false;

    if (s->cursor.page_row) s->cursor.page_row->set_wrap_continuation(true);
    s->cursor.pending_wrap = false;
    return true;
}

/* Deal with a cursor that is sitting past the right margin.
 *
 * A character written at the last column does not move the cursor off the
 * end — there is nowhere to move to. It leaves a flag saying the *next*
 * character wraps first. Keeping it as a flag rather than moving the cursor
 * immediately is what makes a line ending exactly at the margin come out
 * right: the wrap only happens if something else is actually written. */
inline bool screen_resolve_pending_wrap(Screen *s) {
    if (!s->cursor.pending_wrap) return true;

    if (!s->auto_wrap) {
        /* With wrapping off the cursor stays at the margin and each new
         * character overwrites the last one. */
        s->cursor.pending_wrap = false;
        return true;
    }

    return screen_wrap(s);
}

/* Write one codepoint at the cursor and move past it.
 *
 * width is 1 or 2 columns, which the caller works out — how wide a codepoint
 * is depends on Unicode tables and on the terminal's own settings, and this
 * has no business knowing about either. */
inline bool screen_write_codepoint(Screen *s, uint32_t cp, int width) {
    if (width != 1 && width != 2) return false;

    const CellCountInt cols = s->pages.cols;

    /* A screen too narrow to hold a wide character at all. Wrapping would not
     * help — the next row is just as narrow — so there is nowhere for it to
     * go, and the alternative to refusing is writing its spacer past the end
     * of the row. */
    if (width == 2 && cols < 2) return false;

    if (!screen_resolve_pending_wrap(s)) return false;

    if (width == 2) {
        /* A wide character will not be split across the right margin. The
         * column it cannot use is marked so that a reflow later knows the gap
         * was the margin's doing and not something the program printed. */
        if ((size_t)s->cursor.x + 1 >= (size_t)cols) {
            if (!s->auto_wrap) return true;

            screen_erase_for_write(s, s->cursor.x);
            Cell *pad = s->cursor.page_cell;
            *pad = Cell();
            pad->set_wide(Wide::spacer_head);

            if (!screen_wrap(s)) return false;
        }
    }

    screen_erase_for_write(s, s->cursor.x);
    if (!screen_cursor_apply_style(s)) return false;
    if (!screen_cursor_apply_hyperlink(s)) return false;

    Cell *c = s->cursor.page_cell;
    c->set_content_tag(ContentTag::codepoint);
    c->set_codepoint(cp);
    c->set_wide(width == 2 ? Wide::wide : Wide::narrow);
    if (s->cursor.page_row) s->cursor.page_row->set_dirty(true);

    if (width == 2) {
        Pin p = s->cursor.page_pin.pin;
        Cell *tail = p.node->page.get_cell((CellCountInt)(s->cursor.x + 1), p.y);
        *tail = Cell();
        tail->set_wide(Wide::spacer_tail);
    }

    const size_t next = (size_t)s->cursor.x + (size_t)width;
    if (next >= (size_t)cols) {
        /* No column left to move to, so the cursor stays where it is and the
         * next character deals with it. */
        s->cursor.pending_wrap = true;
        return true;
    }

    return screen_cursor_absolute(s, (CellCountInt)next, s->cursor.y);
}

/* Add a codepoint to the character just written.
 *
 * Combining marks arrive after the character they modify, so this attaches to
 * the cell behind the cursor rather than the one under it. A cursor that has
 * not written anything on this row has nothing to attach to. */
inline bool screen_append_grapheme(Screen *s, uint32_t cp) {
    Pin p = s->cursor.page_pin.pin;
    if (!p.valid()) return false;

    /* The cell the last character went into: the one under the cursor when a
     * wrap is pending, otherwise the one before it. */
    CellCountInt x = s->cursor.x;
    if (!s->cursor.pending_wrap) {
        if (x == 0) return false;
        x = (CellCountInt)(x - 1);
    }

    Page *page = &p.node->page;

    /* A wide character's spacer is not where its codepoints live. */
    if (page->get_cell(x, p.y)->wide() == Wide::spacer_tail) {
        if (x == 0) return false;
        x = (CellCountInt)(x - 1);
    }

    if (page_append_grapheme(page, x, p.y, cp)) return true;

    /* Out of grapheme storage: the same answer as running out of styles. */
    PageNode *fresh = page_list_grow_budget(&s->pages, p.node,
                                            PageBudget::grapheme_bytes);
    if (!fresh) return false;

    screen_cursor_reload(s);
    p = s->cursor.page_pin.pin;
    return page_append_grapheme(&p.node->page, x, p.y, cp);
}

/* Write a run of narrow ASCII, which is what almost all output is. */
inline bool screen_write_ascii(Screen *s, const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!screen_write_codepoint(s, (uint32_t)(unsigned char)text[i], 1)) {
            return false;
        }
    }
    return true;
}


/* ─── erasing ────────────────────────────────────────────────────────────── */

/* What an erased cell looks like.
 *
 * Erasing is not quite blanking. A program that sets a background colour and
 * then clears the screen expects the screen to be that colour, so an erased
 * cell carries the current background — which is why erasing with a colour
 * set paints rather than empties.
 *
 * The colour goes straight into the cell rather than through a style. Cell
 * has tags for exactly this, and a screenful of erased cells is the case they
 * were added for: a page's style set is small and an erase touching every
 * cell would otherwise want a slot for a style whose only content is a
 * background. */
inline Cell screen_erased_cell(const Screen *s) {
    Cell c;
    s->cursor.style.bg_cell(&c);
    return c;
}

/* Erase a run of cells on one screen row.
 *
 * selective honours the protection bit, which is what DECSCA marks cells with
 * and what the selective erases are for. Ordinary erases ignore it.
 *
 * The range is widened to whole wide characters at both ends. Erasing one
 * half of a pair and leaving the other is the same broken state that writing
 * over one half would produce, and it is no more acceptable for arriving by a
 * different route. */
inline bool screen_clear_cells(Screen *s, CellCountInt y, CellCountInt x,
                               CellCountInt count, bool selective) {
    if (y >= s->pages.rows || count == 0) return false;

    Pin p = page_list_active_pin(&s->pages, 0, y);
    if (!p.valid()) return false;

    Page *page = &p.node->page;
    const CellCountInt cols = s->pages.cols;
    if (x >= cols) return false;

    size_t from = x;
    size_t to = (size_t)x + count;
    if (to > (size_t)cols) to = cols;

    if (from > 0 && page->get_cell((CellCountInt)from, p.y)->wide() ==
                        Wide::spacer_tail) {
        from--;
    }
    if (to < (size_t)cols && page->get_cell((CellCountInt)(to - 1), p.y)->wide() ==
                                 Wide::wide) {
        to++;
    }

    const Cell blank = screen_erased_cell(s);

    for (size_t i = from; i < to; i++) {
        Cell *c = page->get_cell((CellCountInt)i, p.y);
        if (selective && c->protect()) continue;

        page_erase_cell(page, (CellCountInt)i, p.y);
        *page->get_cell((CellCountInt)i, p.y) = blank;
    }

    Row *row = page->get_row(p.y);
    row->set_dirty(true);

    /* An erase that reaches the end of the row ends the line there: what it
     * continued into is gone. The next row's continuation flag is left as it
     * was, since nothing reads it — a reflow follows the wrap flag on the row
     * that wraps, not the mark on the row that was wrapped into. */
    if (to >= (size_t)cols && !selective) row->set_wrap(false);

    return true;
}

/* Erase whole screen rows, top to bot inclusive. */
inline void screen_clear_rows(Screen *s, CellCountInt top, CellCountInt bot,
                              bool selective) {
    if (bot >= s->pages.rows) bot = (CellCountInt)(s->pages.rows - 1);
    for (CellCountInt y = top; y <= bot; y++) {
        screen_clear_cells(s, y, 0, s->pages.cols, selective);
    }
}

/* EL — erase in line.
 *
 * 0 erases from the cursor to the end of the row, 1 from the start of the row
 * to the cursor, 2 the whole row. All three include the cell the cursor is
 * on, which is what the standard says and what every program expects. */
inline bool screen_erase_line(Screen *s, int mode, bool selective) {
    const CellCountInt cols = s->pages.cols;
    const CellCountInt x = s->cursor.x;
    const CellCountInt y = s->cursor.y;

    bool ok;
    switch (mode) {
        case 0:
            ok = screen_clear_cells(s, y, x, (CellCountInt)(cols - x), selective);
            break;
        case 1:
            ok = screen_clear_cells(s, y, 0, (CellCountInt)(x + 1), selective);
            break;
        case 2:
            ok = screen_clear_cells(s, y, 0, cols, selective);
            break;
        default:
            return false;
    }

    /* Erasing where the cursor sits gives it somewhere to write again, so a
     * wrap it was waiting to do is no longer pending. */
    if (ok) s->cursor.pending_wrap = false;

    screen_cursor_reload(s);
    return ok;
}

/* ECH — erase n characters from the cursor, without moving it.
 *
 * Unlike EL this does not end the line: it clears a hole in the middle of
 * one, and what follows the hole is still part of the same line. */
inline bool screen_erase_chars(Screen *s, CellCountInt n, bool selective) {
    if (n == 0) n = 1;

    const CellCountInt cols = s->pages.cols;
    const CellCountInt x = s->cursor.x;
    const size_t count = (size_t)n > (size_t)(cols - x) ? (size_t)(cols - x) : n;

    Pin p = page_list_active_pin(&s->pages, 0, s->cursor.y);
    if (!p.valid()) return false;
    const bool wrapped = p.row()->wrap();

    const bool ok = screen_clear_cells(s, s->cursor.y, x, (CellCountInt)count,
                                       selective);
    if (ok) {
        if (wrapped) p.row()->set_wrap(true);
        s->cursor.pending_wrap = false;
    }

    screen_cursor_reload(s);
    return ok;
}

/* ED — erase in display.
 *
 * 0 erases from the cursor to the bottom of the screen, 1 from the top to the
 * cursor, 2 the whole screen. 3 is the xterm extension that throws away the
 * scrollback as well, and it is the only one that destroys anything a user
 * could still have scrolled back to see. */
inline bool screen_erase_display(Screen *s, int mode, bool selective) {
    const CellCountInt rows = s->pages.rows;
    const CellCountInt cols = s->pages.cols;
    const CellCountInt y = s->cursor.y;

    switch (mode) {
        case 0:
            screen_clear_cells(s, y, s->cursor.x,
                               (CellCountInt)(cols - s->cursor.x), selective);
            if (y + 1 < rows) {
                screen_clear_rows(s, (CellCountInt)(y + 1),
                                  (CellCountInt)(rows - 1), selective);
            }
            break;

        case 1:
            if (y > 0) screen_clear_rows(s, 0, (CellCountInt)(y - 1), selective);
            screen_clear_cells(s, y, 0, (CellCountInt)(s->cursor.x + 1),
                               selective);
            break;

        case 2:
            screen_clear_rows(s, 0, (CellCountInt)(rows - 1), selective);
            break;

        case 3:
            /* The screen itself is left alone; only what has scrolled off it
             * goes. */
            page_list_erase_scrollback(&s->pages);
            break;

        default:
            return false;
    }

    s->cursor.pending_wrap = false;
    screen_cursor_reload(s);
    return true;
}


/* ─── scrolling a region ─────────────────────────────────────────────────── */

/* Whether every row of a screen range lives in one page.
 *
 * Worth asking because the answer decides which of two quite different
 * operations runs. Rows inside a page move by swapping 64-bit handles and
 * nothing else happens; rows spanning pages have to be copied, which means
 * re-interning every style and link they carry. Most scroll regions are small
 * and most pages hold hundreds of rows, so the cheap path is the usual one. */
inline PageNode *screen_rows_in_one_page(Screen *s, CellCountInt top,
                                         CellCountInt bot,
                                         CellCountInt *page_top) {
    Pin a = page_list_active_pin(&s->pages, 0, top);
    Pin b = page_list_active_pin(&s->pages, 0, bot);
    if (!a.valid() || !b.valid() || a.node != b.node) return nullptr;

    *page_top = a.y;
    return a.node;
}

/* Move the rows of a region up, blanking the ones that arrive at the bottom.
 *
 * This is the scroll that happens inside a screen, with no scrollback
 * involved: what leaves the top of the region is gone. A caller that wants
 * the departing line kept has to be the whole-screen case, which is not this
 * function's business. */
inline bool screen_scroll_region_up(Screen *s, CellCountInt top,
                                    CellCountInt bot, CellCountInt n) {
    if (n == 0 || top > bot || bot >= s->pages.rows) return false;

    const CellCountInt count = (CellCountInt)(bot - top + 1);
    if (n >= count) {
        screen_clear_rows(s, top, bot, false);
        return true;
    }

    CellCountInt page_top = 0;
    PageNode *node = screen_rows_in_one_page(s, top, bot, &page_top);
    if (node) {
        page_scroll_up(&node->page, page_top,
                       (CellCountInt)(page_top + count - 1), n);
        screen_cursor_reload(s);
        return true;
    }

    /* Across a page boundary the rows have to be copied, since a row's cells
     * belong to the page they were allocated in. */
    for (CellCountInt y = top; (CellCountInt)(y + n) <= bot; y++) {
        Pin dst = page_list_active_pin(&s->pages, 0, y);
        Pin src = page_list_active_pin(&s->pages, 0, (CellCountInt)(y + n));
        if (!dst.valid() || !src.valid()) return false;
        if (!page_clone_row(&dst.node->page, dst.y, &src.node->page, src.y)) {
            return false;
        }
    }

    screen_clear_rows(s, (CellCountInt)(bot - n + 1), bot, false);
    screen_cursor_reload(s);
    return true;
}

/* The same downwards: rows move toward the bottom and blank ones arrive at
 * the top. What falls off the bottom of the region is gone. */
inline bool screen_scroll_region_down(Screen *s, CellCountInt top,
                                      CellCountInt bot, CellCountInt n) {
    if (n == 0 || top > bot || bot >= s->pages.rows) return false;

    const CellCountInt count = (CellCountInt)(bot - top + 1);
    if (n >= count) {
        screen_clear_rows(s, top, bot, false);
        return true;
    }

    CellCountInt page_top = 0;
    PageNode *node = screen_rows_in_one_page(s, top, bot, &page_top);
    if (node) {
        page_scroll_down(&node->page, page_top,
                         (CellCountInt)(page_top + count - 1), n);
        screen_cursor_reload(s);
        return true;
    }

    for (CellCountInt y = bot; y >= (CellCountInt)(top + n); y--) {
        Pin dst = page_list_active_pin(&s->pages, 0, y);
        Pin src = page_list_active_pin(&s->pages, 0, (CellCountInt)(y - n));
        if (!dst.valid() || !src.valid()) return false;
        if (!page_clone_row(&dst.node->page, dst.y, &src.node->page, src.y)) {
            return false;
        }
        if (y == 0) break;
    }

    screen_clear_rows(s, top, (CellCountInt)(top + n - 1), false);
    screen_cursor_reload(s);
    return true;
}

/* ─── resizing ───────────────────────────────────────────────────────────── */

/* Drop the blank rows below the cursor before a reflow.
 *
 * A screen is almost always mostly empty below the cursor, and those blank
 * rows are not text — they are the rest of the screen. Reflowing them anyway
 * means a line above the cursor that grows from one row to four pushes the
 * cursor off the top of the screen, because the blanks below it still take up
 * the room they used to.
 *
 * So they go first. This is what terminals have always done on a resize, and
 * it belongs here rather than in the list: only a screen knows where its
 * cursor is, and without a cursor there is no way to tell a blank row that is
 * the end of the screen from a blank line somebody printed.
 *
 * Only genuinely empty rows at the very end go, never one that is part of a
 * wrapped line, and never one the cursor or anything else is pinned to. */
inline void screen_trim_blank_rows_below_cursor(Screen *s) {
    const size_t cursor_at =
        page_list_row_index(&s->pages, s->cursor.page_pin.pin);
    if (cursor_at == (size_t)-1) return;

    while (s->pages.row_count > cursor_at + 1) {
        PageNode *last = s->pages.last;
        if (!last || last->rows_used == 0) break;

        const CellCountInt y = (CellCountInt)(last->rows_used - 1);
        Row *row = last->page.get_row(y);

        if (page_row_used_width(&last->page, y) != 0) break;
        if (row->wrap() || row->wrap_continuation()) break;

        /* Something is pinned here, so it is not spare room. */
        bool pinned = false;
        for (TrackedPin *t = s->pages.tracked; t; t = t->next) {
            if (t->pin.node == last && t->pin.y == y) {
                pinned = true;
                break;
            }
        }
        if (pinned) break;

        page_clear_row(&last->page, y);
        last->rows_used--;
        s->pages.row_count--;
    }
}

/* Resize the screen, keeping the cursor on the character it was on.
 *
 * The cursor is a tracked pin, so the reflow moves it; what it does not do is
 * work out which screen coordinates that lands on, because the list has no
 * idea what the cursor is. That is recovered here from the pin's own
 * position. */
inline bool screen_resize(Screen *s, CellCountInt cols, CellCountInt rows) {
    if (cols == 0 || rows == 0) return false;

    if (cols != s->pages.cols) screen_trim_blank_rows_below_cursor(s);

    if (!page_list_resize(&s->pages, cols, rows)) return false;

    Pin p = s->cursor.page_pin.pin;
    if (!p.valid()) {
        page_list_track(&s->pages, &s->cursor.page_pin,
                        page_list_active_start(&s->pages));
        return screen_cursor_absolute(s, 0, 0);
    }

    /* Where the pin ended up, expressed as a screen coordinate again. A pin
     * that reflowed into the scrollback — which is where a cursor near the
     * top of a screen that just got much narrower goes — is clamped to the
     * top of the active area, since a cursor outside the screen is not a
     * thing a terminal can have. */
    const size_t at = page_list_row_index(&s->pages, p);
    const size_t top = page_list_max_scroll(&s->pages);

    CellCountInt y = 0;
    if (at != (size_t)-1 && at > top) {
        const size_t rel = at - top;
        y = (CellCountInt)(rel < (size_t)s->pages.rows
                               ? rel
                               : (size_t)(s->pages.rows - 1));
    }

    CellCountInt x = p.x;
    if (x >= s->pages.cols) x = (CellCountInt)(s->pages.cols - 1);

    return screen_cursor_absolute(s, x, y);
}

/* ─── selections ────────────────────────────────────────────────────── */
/* Select from one pin to another. */
inline void screen_select(Screen *s, const Pin &start, const Pin &end,
                          bool rectangle) {
    if (!start.valid() || !end.valid()) return;

    screen_select_clear(s);

    page_list_track(&s->pages, &s->selection.start_pin, start);
    page_list_track(&s->pages, &s->selection.end_pin, end);
    s->selection.rectangle = rectangle;
    s->selection.active = true;
}

/* The two ends, top-left first.
 *
 * For a rectangle the columns are ordered too, since a block dragged leftwards
 * still covers the columns between its edges. For a linear selection the
 * column belongs to its own row and is left alone. */
inline bool screen_selection_ordered(Screen *s, Pin *tl, Pin *br) {
    if (!s->selection.active) return false;

    Pin a = s->selection.start_pin.pin;
    Pin b = s->selection.end_pin.pin;
    if (!a.valid() || !b.valid()) return false;

    const size_t ia = page_list_row_index(&s->pages, a);
    const size_t ib = page_list_row_index(&s->pages, b);
    if (ia == (size_t)-1 || ib == (size_t)-1) return false;

    const bool swap = ib < ia || (ia == ib && b.x < a.x);
    *tl = swap ? b : a;
    *br = swap ? a : b;

    if (s->selection.rectangle && tl->x > br->x) {
        const CellCountInt t = tl->x;
        tl->x = br->x;
        br->x = t;
    }

    return true;
}

/* Whether a cell is inside the selection. This is what a renderer asks, once
 * per cell, so it answers from row indices rather than by walking. */
inline bool screen_selection_contains(Screen *s, const Pin &p) {
    Pin tl, br;
    if (!screen_selection_ordered(s, &tl, &br)) return false;
    if (!p.valid()) return false;

    const size_t at = page_list_row_index(&s->pages, p);
    if (at == (size_t)-1) return false;

    const size_t top = page_list_row_index(&s->pages, tl);
    const size_t bot = page_list_row_index(&s->pages, br);
    if (at < top || at > bot) return false;

    if (s->selection.rectangle) return p.x >= tl.x && p.x <= br.x;

    if (at == top && p.x < tl.x) return false;
    if (at == bot && p.x > br.x) return false;
    return true;
}

/* Extend a selection over the whole soft-wrapped line a pin is on.
 *
 * A line is what the program printed, not what the screen happened to break
 * it into, so this walks the wrap flags in both directions. */
inline bool screen_select_line(Screen *s, const Pin &p) {
    if (!p.valid()) return false;

    Pin start = p;
    while (true) {
        Pin above = start;
        if (!above.up(1)) break;
        if (!above.row()->wrap()) break;
        start = above;
    }

    Pin end = p;
    while (end.row()->wrap()) {
        Pin below = end;
        if (!below.down(1)) break;
        end = below;
    }

    start.x = 0;
    end.x = (CellCountInt)(s->pages.cols - 1);
    screen_select(s, start, end, false);
    return true;
}

/* Select everything the list holds, scrollback included. */
inline bool screen_select_all(Screen *s) {
    Pin start = page_list_pin(&s->pages, 0);
    if (!start.valid() || s->pages.row_count == 0) return false;

    Pin end = page_list_pin(&s->pages, s->pages.row_count - 1);
    if (!end.valid()) return false;

    start.x = 0;
    end.x = (CellCountInt)(s->pages.cols - 1);
    screen_select(s, start, end, false);
    return true;
}

/* ─── reading a selection out ────────────────────────────────────────────── */

/* Encode one codepoint as UTF-8, returning how many bytes it took. */
inline size_t screen_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The selected text, as UTF-8.
 *
 * The rule that matters is the newlines. A line too long for the screen is
 * several rows joined by a wrap flag, and pasting it back should give what
 * the program printed — one line — not the shape the screen happened to break
 * it into. So a newline goes in at a hard line end and nowhere else. This is
 * the same distinction reflow turns on, used for the thing a user actually
 * notices.
 *
 * Trailing blanks on a row are dropped, since they are the rest of the screen
 * rather than spaces anybody typed. A rectangle keeps its columns as they are:
 * the whole point of a block selection is the shape.
 *
 * Returns the number of bytes written, and writes nothing beyond cap. */
inline size_t screen_selection_text(Screen *s, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return 0;
    out[0] = '\0';

    Pin tl, br;
    if (!screen_selection_ordered(s, &tl, &br)) return 0;

    const size_t top = page_list_row_index(&s->pages, tl);
    const size_t bot = page_list_row_index(&s->pages, br);
    if (top == (size_t)-1 || bot == (size_t)-1) return 0;

    const CellCountInt cols = s->pages.cols;
    Pin row = tl;

    for (size_t at = top; at <= bot; at++) {
        Page *page = &row.node->page;

        CellCountInt from = 0;
        CellCountInt to = cols;

        if (s->selection.rectangle) {
            from = tl.x;
            to = (CellCountInt)(br.x + 1);
        } else {
            if (at == top) from = tl.x;
            if (at == bot) to = (CellCountInt)(br.x + 1);
        }

        /* Trailing blanks are the rest of the screen, not text. */
        CellCountInt width = to;
        while (width > from &&
               page_cell_is_blank(page->get_cell((CellCountInt)(width - 1),
                                                 row.y))) {
            width--;
        }

        for (CellCountInt x = from; x < width; x++) {
            Cell *c = page->get_cell(x, row.y);
            if (c->wide() == Wide::spacer_tail ||
                c->wide() == Wide::spacer_head) {
                continue;
            }

            const uint32_t cp = c->codepoint();
            if (cp == 0) {
                if (n + 1 < cap) out[n++] = ' ';
                continue;
            }

            char buf[4];
            const size_t len = screen_utf8_encode(cp, buf);
            if (n + len >= cap) break;
            for (size_t i = 0; i < len; i++) out[n++] = buf[i];

            /* A cluster's remaining codepoints live in the page, not the
             * cell, and they are as much a part of the character as the one
             * that is. */
            const uint32_t *extra = nullptr;
            uint32_t extra_len = 0;
            if (c->has_grapheme() &&
                page_grapheme_codepoints(page, x, row.y, &extra, &extra_len)) {
                for (uint32_t i = 0; i < extra_len; i++) {
                    const size_t elen = screen_utf8_encode(extra[i], buf);
                    if (n + elen >= cap) break;
                    for (size_t j = 0; j < elen; j++) out[n++] = buf[j];
                }
            }
        }

        /* A soft wrap is not a line ending — the row below is the same line,
         * and a newline here is the shape of the screen leaking into the
         * text. A rectangle is all line endings, since its rows are not
         * joined to each other in any sense. */
        const bool last = at == bot;
        const bool soft = !s->selection.rectangle && row.row()->wrap();
        if (!last && !soft && n + 1 < cap) out[n++] = '\n';

        if (last) break;
        if (!row.down(1)) break;
    }

    out[n] = '\0';
    return n;
}

inline void screen_save_cursor(Screen *s) {
    s->saved.x = s->cursor.x;
    s->saved.y = s->cursor.y;
    s->saved.style = s->cursor.style;
    s->saved.pending_wrap = s->cursor.pending_wrap;
    s->saved.valid = true;
}

/* Put the cursor back where DECSC left it.
 *
 * The saved position can be off the screen by now — the window may have been
 * made smaller since — so it is clamped rather than refused. A restore that
 * did nothing would leave the cursor somewhere the program has no reason to
 * expect, which is worse than putting it as close as the screen allows. */
inline bool screen_restore_cursor(Screen *s) {
    if (!s->saved.valid) return screen_cursor_absolute(s, 0, 0);

    CellCountInt x = s->saved.x;
    CellCountInt y = s->saved.y;
    if (x >= s->pages.cols) x = (CellCountInt)(s->pages.cols - 1);
    if (y >= s->pages.rows) y = (CellCountInt)(s->pages.rows - 1);

    if (!screen_cursor_absolute(s, x, y)) return false;

    s->cursor.style = s->saved.style;
    s->cursor.pending_wrap = s->saved.pending_wrap && x == s->saved.x;
    return true;
}

/* ─── selecting by word ──────────────────────────────────────────────────── */

/* Whether a cell holds part of a word.
 *
 * The rule is whitespace against everything else, which is coarse but is what
 * a double click means: take the run of things that are not gaps. Punctuation
 * counts as part of a word, so a path or a URL comes out in one piece, which
 * is what a user double-clicking one is after.
 *
 * An empty cell is a gap. So is a wide character's spacer as far as the class
 * goes, but a spacer is never the boundary — it is carried along with its
 * lead. */
inline bool screen_cell_is_word(const Cell *c) {
    if (c->wide() == Wide::spacer_tail) return true;
    const uint32_t cp = c->codepoint();
    if (cp == 0) return false;
    return cp != ' ' && cp != '\t';
}

/* Select the run of word or of whitespace that a pin is in.
 *
 * Words wrapped across rows are one word: the run follows the wrap flags, so
 * a long path broken by the screen's width still selects whole. A hard line
 * end stops it, because that is a line the program ended and the next line is
 * not a continuation of this word. */
inline bool screen_select_word(Screen *s, const Pin &p) {
    if (!p.valid()) return false;

    Pin at = p;
    Page *page = &at.node->page;

    /* A spacer is never a boundary; it belongs to the character in front. */
    if (at.x > 0 && page->get_cell(at.x, at.y)->wide() == Wide::spacer_tail) {
        at.x = (CellCountInt)(at.x - 1);
    }

    const bool want = screen_cell_is_word(at.node->page.get_cell(at.x, at.y));
    const CellCountInt cols = s->pages.cols;

    Pin start = at;
    while (true) {
        if (start.x > 0) {
            Cell *c = start.node->page.get_cell((CellCountInt)(start.x - 1),
                                                start.y);
            if (screen_cell_is_word(c) != want) break;
            start.x = (CellCountInt)(start.x - 1);
            continue;
        }

        /* Column zero: the word may continue on the row above, but only if
         * that row wrapped into this one. */
        Pin above = start;
        if (!above.up(1)) break;
        if (!above.row()->wrap()) break;

        above.x = (CellCountInt)(cols - 1);
        if (screen_cell_is_word(above.cell()) != want) break;
        start = above;
    }

    Pin end = at;
    while (true) {
        if (end.x + 1 < cols) {
            Cell *c = end.node->page.get_cell((CellCountInt)(end.x + 1), end.y);
            if (screen_cell_is_word(c) != want) break;
            end.x = (CellCountInt)(end.x + 1);
            continue;
        }

        if (!end.row()->wrap()) break;
        Pin below = end;
        if (!below.down(1)) break;

        below.x = 0;
        if (screen_cell_is_word(below.cell()) != want) break;
        end = below;
    }

    screen_select(s, start, end, false);
    return true;
}

/* ─── reading ────────────────────────────────────────────────────────────── */

/* The cell at a screen coordinate, or null if it is off the screen. */
inline Cell *screen_cell(Screen *s, CellCountInt x, CellCountInt y) {
    if (x >= s->pages.cols || y >= s->pages.rows) return nullptr;
    Pin p = page_list_active_pin(&s->pages, x, y);
    return p.valid() ? p.cell() : nullptr;
}

/* The row at a screen coordinate, or null if it is off the screen. */
inline Row *screen_row(Screen *s, CellCountInt y) {
    if (y >= s->pages.rows) return nullptr;
    Pin p = page_list_active_pin(&s->pages, 0, y);
    return p.valid() ? p.row() : nullptr;
}

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SCREEN_HPP */
