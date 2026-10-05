/* Reimplemented after Ghostty src/terminal/Terminal.zig
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
 * The terminal: the thing a program is actually talking to.
 *
 * A Screen is a buffer with a cursor over it, and it has no opinions. It does
 * not know that a line feed is different from moving down, that a tab goes to
 * the next tab stop, or that DECAWM exists. Those are properties of a
 * terminal — of the machine being emulated — and this is where they live.
 *
 * Two things arrive here that nothing below could hold:
 *
 * The modes. A terminal's behaviour is settable by escape sequence, and a
 * mode is not state about the text but state about what the next character
 * will do to it. auto_wrap has been sitting on Screen since writing text was
 * ported, with a note saying it belonged here; this is where it moves to, and
 * the Screen's copy becomes something Terminal keeps in step rather than
 * something anyone else sets.
 *
 * The second screen. A terminal owns two: the primary one, with its
 * scrollback, and the alternate one that full-screen programs draw on. They
 * are the same kind of thing, which is why a Screen has no idea there is
 * another — swapping between them is the terminal's business.
 *
 * NOT THE LIVE TERMINAL. Wisp's working emulator is the C code under
 * src/terminal. This is the ported one, built on the ported screen, and
 * nothing runs on it yet.
 *
 * PARTIAL PORT. The terminal, its modes, both screens, printing, the C0
 * control characters, tab stops, scroll regions, the insert and delete
 * operations and the character sets are here. The rest of the modes are not,
 * so the ledger records Terminal.zig as `wip`.
 */

#pragma once
#ifndef WISP_TERMINAL_TERMINAL_HPP
#define WISP_TERMINAL_TERMINAL_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "screen.hpp"
#include "charsets.hpp"

namespace wisp {
namespace terminal {

/* ─── modes ──────────────────────────────────────────────────────────────── */

/* The settable behaviours, by their DEC numbers where they have them.
 *
 * Only the ones something already does are here. A mode nobody reads would be
 * a field that looks like it works. */
struct Modes {
    /* DECAWM (?7): a character at the right margin wraps to the next line
     * rather than overwriting the last column. On by default, which is what
     * every terminal has done since the VT100. */
    bool wraparound;

    /* IRM (4): printing pushes what is already on the line to the right
     * rather than writing over it. */
    bool insert;

    /* DECCKM (?1): the arrow keys send application sequences. Nothing here
     * generates key sequences yet, so this is carried rather than used —
     * it is set by the same escape sequence as the others and losing it
     * would mean a program that sets it and asks for it back getting a
     * different answer. */
    bool cursor_keys;

    /* DECOM (?6): the cursor is positioned relative to the scroll region
     * rather than to the screen, and cannot leave it. */
    bool origin;

    /* ?1049: the alternate screen is showing. Kept as a mode because that is
     * how a program asks about it, but the truth is which screen is active. */
    bool alt_screen;

    Modes()
        : wraparound(true), insert(false), cursor_keys(false), origin(false),
          alt_screen(false) {}
};

/* ─── character sets ─────────────────────────────────────────────────────── */

/* Which sets are loaded, and which one is in use.
 *
 * Four slots, G0 to G3, each holding a set. A program loads a set into a
 * slot with one sequence and switches to the slot with another, which is why
 * there are two steps rather than "use this set": a program could load line
 * drawing into G1 once and then flip between G0 and G1 with a single byte
 * each time, which mattered when bytes cost something.
 *
 * gl is the slot ordinary text is printed from — G0 until SO says G1.
 * single_shift is SS2 or SS3, which use G2 or G3 for exactly one character
 * and then go back. */
struct CharsetState {
    Charset slots[4];
    uint8_t gl;

    /* 0 when none is pending, otherwise 2 or 3. */
    uint8_t single_shift;

    CharsetState() : gl(0), single_shift(0) {
        for (int i = 0; i < 4; i++) slots[i] = Charset::ascii;
    }
};

/* ─── the terminal ───────────────────────────────────────────────────────── */

/* How far apart tab stops are when there is nothing better to say.
 *
 * Every terminal since the teletype has used eight, and programs that draw
 * columns assume it. */
static const CellCountInt TAB_INTERVAL = 8;

struct Terminal {
    /* Both screens exist for the terminal's whole life. Making the alternate
     * one on demand would mean an allocation failing in the middle of an
     * escape sequence, which is not a failure anything could do something
     * useful with. */
    Screen primary;
    Screen alternate;

    /* Which of the two is being written to. */
    Screen *active;

    Modes modes;

    CellCountInt cols;
    CellCountInt rows;

    /* DECSTBM, inclusive and counted from the top of the screen. Set to the
     * whole screen until a program says otherwise. */
    CellCountInt scroll_top;
    CellCountInt scroll_bot;

    CharsetState charsets;

    /* The character sets as DECSC left them. They are saved alongside the
     * cursor because a program that saves the cursor, switches to line
     * drawing to draw a box and restores expects to be printing text again
     * afterwards — the saved cursor has noted since it was ported that this
     * part of it belonged to the terminal, and this is where it arrives. */
    CharsetState saved_charsets;

    /* What OSC 0 and 2 last set, for the window. Kept here rather than
     * applied anywhere, because a terminal emulator has no window — whatever
     * hosts it reads this. */
    char   title[256];
    size_t title_len;

    /* Handed out to OSC 8 links that did not name themselves, so that two
     * separate runs linking the same URI stay two links rather than merging
     * into one. That distinction is what hyperlink.hpp's implicit IDs are
     * for. */
    uint32_t next_implicit_link;

    /* One byte per column: is there a tab stop here. A bitset would be eight
     * times smaller and a great deal less obvious, for a few hundred bytes. */
    uint8_t *tabs;

    Terminal()
        : primary(), alternate(), active(nullptr), modes(), cols(0), rows(0),
          scroll_top(0), scroll_bot(0), charsets(), saved_charsets(),
          title_len(0), next_implicit_link(0), tabs(nullptr) {
        title[0] = '\0';
    }
};

/* Put tab stops back to every eighth column. */
inline void terminal_reset_tabs(Terminal *t) {
    if (!t->tabs) return;
    memset(t->tabs, 0, t->cols);
    for (CellCountInt x = TAB_INTERVAL; x < t->cols; x += TAB_INTERVAL) {
        t->tabs[x] = 1;
    }
}

inline void terminal_deinit(Terminal *t) {
    screen_deinit(&t->primary);
    screen_deinit(&t->alternate);
    free(t->tabs);
    t->tabs = nullptr;
    t->active = nullptr;
}

inline bool terminal_init(Terminal *t, CellCountInt cols, CellCountInt rows,
                          size_t max_scrollback_bytes) {
    *t = Terminal();
    t->cols = cols;
    t->rows = rows;

    if (!screen_init(&t->primary, cols, rows, max_scrollback_bytes)) {
        return false;
    }

    /* The alternate screen keeps nothing. Scrollback there would be a record
     * of half-drawn frames rather than of output, since the programs that use
     * it redraw the whole display whenever they feel like it. */
    if (!screen_init(&t->alternate, cols, rows, 0)) {
        screen_deinit(&t->primary);
        return false;
    }
    t->alternate.pages.no_scrollback = true;

    t->tabs = (uint8_t *)malloc(cols);
    if (!t->tabs) {
        terminal_deinit(t);
        return false;
    }
    terminal_reset_tabs(t);

    t->scroll_top = 0;
    t->scroll_bot = (CellCountInt)(rows - 1);

    t->active = &t->primary;
    t->active->auto_wrap = t->modes.wraparound;
    return true;
}

/* Defined with the scroll region, which is what decides where home is. */
inline bool terminal_cursor_position(Terminal *t, CellCountInt x,
                                     CellCountInt y);

/* ─── modes ──────────────────────────────────────────────────────────────── */

/* Push the modes that a screen needs to know about down to it.
 *
 * Only wraparound so far. The screen has a copy rather than a pointer because
 * writing a character reads it once per character, and because a Screen that
 * reached back up into a Terminal would be the wrong way round. */
inline void terminal_apply_modes(Terminal *t) {
    t->primary.auto_wrap = t->modes.wraparound;
    t->alternate.auto_wrap = t->modes.wraparound;
}

/* ─── the alternate screen ───────────────────────────────────────────────── */

/* Switch to the alternate screen, as ?1049 does.
 *
 * The cursor is saved on the way in and the alternate screen is cleared, so a
 * program gets a blank display and the shell gets its prompt back afterwards
 * exactly where it was. Doing the save here rather than leaving it to the
 * program is the difference between ?1049 and the older ?47, and it is why
 * ?1049 is the one everything uses. */
inline void terminal_alt_screen_enter(Terminal *t) {
    if (t->modes.alt_screen) return;

    screen_save_cursor(&t->primary);
    t->saved_charsets = t->charsets;

    t->active = &t->alternate;
    t->modes.alt_screen = true;

    screen_select_clear(&t->alternate);
    screen_cursor_absolute(&t->alternate, 0, 0);
    t->alternate.cursor.style = style::Style();
    screen_erase_display(&t->alternate, 2, false);
    screen_cursor_absolute(&t->alternate, 0, 0);
}

/* Switch back, restoring the cursor the way in put away. */
inline void terminal_alt_screen_leave(Terminal *t) {
    if (!t->modes.alt_screen) return;

    t->active = &t->primary;
    t->modes.alt_screen = false;

    screen_restore_cursor(&t->primary);
    t->charsets = t->saved_charsets;
}

/* ─── character sets ─────────────────────────────────────────────────────── */

/* ESC ( X and its relatives: load a set into a slot. */
inline void terminal_designate_charset(Terminal *t, int slot, uint8_t designator) {
    if (slot < 0 || slot > 3) return;
    Charset cs;
    if (charset_from_designator(designator, &cs)) t->charsets.slots[slot] = cs;
}

/* SI and SO: print from G0 or G1. */
inline void terminal_shift_in(Terminal *t) { t->charsets.gl = 0; }
inline void terminal_shift_out(Terminal *t) { t->charsets.gl = 1; }

/* SS2 and SS3: the next character only comes from G2 or G3. */
inline void terminal_single_shift(Terminal *t, uint8_t slot) {
    if (slot == 2 || slot == 3) t->charsets.single_shift = slot;
}

/* The codepoint a printed character becomes under the sets in force.
 *
 * A single shift applies to this character and is then used up, which is the
 * whole difference between it and a shift. */
inline uint32_t terminal_map_charset(Terminal *t, uint32_t cp) {
    uint8_t slot = t->charsets.gl;
    if (t->charsets.single_shift) {
        slot = t->charsets.single_shift;
        t->charsets.single_shift = 0;
    }
    return charset_map(t->charsets.slots[slot], cp);
}

/* DECSC and DECRC for the whole terminal: the cursor, and the sets. */
inline void terminal_save_cursor(Terminal *t) {
    screen_save_cursor(t->active);
    t->saved_charsets = t->charsets;
}

inline void terminal_restore_cursor(Terminal *t) {
    screen_restore_cursor(t->active);
    t->charsets = t->saved_charsets;
}

/* ─── printing ───────────────────────────────────────────────────────────── */

/* Print a character.
 *
 * width is how many columns the codepoint occupies, which depends on Unicode
 * tables the terminal is configured with rather than on anything here. */
inline bool terminal_print(Terminal *t, uint32_t cp, int width) {
    Screen *s = t->active;
    cp = terminal_map_charset(t, cp);

    if (t->modes.insert) {
        /* IRM: what is on the line moves right to make room, and whatever
         * falls off the end is gone. */
        const CellCountInt x = s->cursor.x;
        if ((size_t)x + width < (size_t)t->cols) {
            Pin p = page_list_active_pin(&s->pages, 0, s->cursor.y);
            if (p.valid()) {
                Page *page = &p.node->page;
                for (size_t i = t->cols; i-- > (size_t)x + width;) {
                    page_erase_cell(page, (CellCountInt)i, p.y);
                    if (!page_clone_cell(page, (CellCountInt)i, p.y, page,
                                         (CellCountInt)(i - width), p.y)) {
                        break;
                    }
                }
            }
            for (int i = 0; i < width; i++) {
                screen_clear_cells(s, s->cursor.y, (CellCountInt)(x + i), 1,
                                   false);
            }
            screen_cursor_reload(s);
        }
    }

    return screen_write_codepoint(s, cp, width);
}

/* Attach a codepoint to the character just printed. */
inline bool terminal_print_combining(Terminal *t, uint32_t cp) {
    return screen_append_grapheme(t->active, cp);
}

/* ─── the C0 controls ────────────────────────────────────────────────────── */

/* CR — back to the first column, same row. */
inline void terminal_carriage_return(Terminal *t) {
    Screen *s = t->active;
    screen_cursor_absolute(s, 0, s->cursor.y);
}

/* BS — back one column, stopping at the left margin. */
inline void terminal_backspace(Terminal *t) {
    screen_cursor_left(t->active, 1);
}

/* ─── tab stops ──────────────────────────────────────────────────────────── */

/* HTS — a tab stop here. */
inline void terminal_tab_set(Terminal *t) {
    if (t->active->cursor.x < t->cols) t->tabs[t->active->cursor.x] = 1;
}

/* TBC — clear the stop here (0) or every stop (3). */
inline void terminal_tab_clear(Terminal *t, int mode) {
    if (mode == 0) {
        if (t->active->cursor.x < t->cols) t->tabs[t->active->cursor.x] = 0;
    } else if (mode == 3) {
        memset(t->tabs, 0, t->cols);
    }
}

/* HT — forward to the next tab stop, or to the last column if there is none.
 *
 * A tab moves the cursor; it does not write spaces. Anything already in the
 * cells it passes over stays there, which is how a program can tab back over
 * its own output and have it still be visible. */
inline void terminal_horizontal_tab(Terminal *t, CellCountInt n) {
    if (n == 0) n = 1;
    Screen *s = t->active;

    for (CellCountInt i = 0; i < n; i++) {
        CellCountInt x = s->cursor.x;
        CellCountInt to = (CellCountInt)(t->cols - 1);

        for (CellCountInt c = (CellCountInt)(x + 1); c < t->cols; c++) {
            if (t->tabs[c]) {
                to = c;
                break;
            }
        }

        screen_cursor_absolute(s, to, s->cursor.y);
        if (to == (CellCountInt)(t->cols - 1)) break;
    }
}

/* CBT — back to the previous tab stop. */
inline void terminal_reverse_tab(Terminal *t, CellCountInt n) {
    if (n == 0) n = 1;
    Screen *s = t->active;

    for (CellCountInt i = 0; i < n; i++) {
        CellCountInt to = 0;
        for (CellCountInt c = s->cursor.x; c-- > 0;) {
            if (t->tabs[c]) {
                to = c;
                break;
            }
        }

        screen_cursor_absolute(s, to, s->cursor.y);
        if (to == 0) break;
    }
}


/* ─── the scroll region ──────────────────────────────────────────────────── */

/* DECSTBM — the rows a scroll moves.
 *
 * Both bounds are inclusive and counted from the top of the screen. A region
 * is what lets a program keep a status line still while the rest of the
 * display scrolls, and it is why a line feed cannot simply mean "move down or
 * grow the list".
 *
 * Both bounds are counted from zero, like every other coordinate here. The
 * escape sequence numbers its rows from one; converting is the job of
 * whatever parses it, and doing it here would mean one of the two bounds
 * being in different units from the cursor beside it.
 *
 * Setting a region homes the cursor, which is in the standard and is relied
 * on: programs set the region and then draw from the top without a separate
 * positioning sequence. A region that makes no sense — bottom above top, or
 * off the screen — is ignored entirely rather than clamped, which is what
 * every real terminal does. */
inline bool terminal_set_scroll_region(Terminal *t, CellCountInt top,
                                       CellCountInt bot) {
    if (bot >= t->rows) bot = (CellCountInt)(t->rows - 1);
    if (top >= bot) return false;

    t->scroll_top = top;
    t->scroll_bot = bot;

    terminal_cursor_position(t, 0, 0);
    return true;
}

/* Whether the region is the whole screen.
 *
 * This is the distinction a line feed turns on. With the whole screen
 * scrolling, the row leaving the top is the oldest thing the terminal has
 * shown and belongs in the scrollback. With a region set, the row leaving the
 * top of the region is being scrolled past by a program that is managing its
 * own display, and it has not left the screen at all — the rows below the
 * region are still showing. Putting it in the scrollback would fill the
 * history with the middle frames of a progress bar. */
inline bool terminal_region_is_whole_screen(const Terminal *t) {
    return t->scroll_top == 0 && t->scroll_bot == (CellCountInt)(t->rows - 1);
}

/* Where row 0 is as far as the cursor is concerned.
 *
 * DECOM makes positioning relative to the region, so a program that sets a
 * region can address it from 1 without knowing where on the screen it put it.
 * With the mode off the screen is addressed as a whole, region or not. */
inline CellCountInt terminal_origin_row(const Terminal *t) {
    return t->modes.origin ? t->scroll_top : 0;
}

inline CellCountInt terminal_origin_bottom(const Terminal *t) {
    return t->modes.origin ? t->scroll_bot : (CellCountInt)(t->rows - 1);
}

/* CUP — put the cursor somewhere, in whatever coordinates are in force. */
inline bool terminal_cursor_position(Terminal *t, CellCountInt x,
                                     CellCountInt y) {
    const CellCountInt base = terminal_origin_row(t);
    const CellCountInt last = terminal_origin_bottom(t);

    size_t row = (size_t)base + y;
    if (row > (size_t)last) row = last;
    if (x >= t->cols) x = (CellCountInt)(t->cols - 1);

    return screen_cursor_absolute(t->active, x, (CellCountInt)row);
}

/* ─── line feeds inside a region ─────────────────────────────────────────── */

/* LF and IND, now that a region can exist.
 *
 * Three cases, and they are genuinely different. At the bottom of a region
 * that is not the whole screen the region scrolls and the departing row is
 * dropped. At the bottom of a full-screen region the list grows and the
 * departing row becomes scrollback. Anywhere else the cursor just moves
 * down — including below the region, where a cursor that has been left
 * outside is not scrolling anything. */
inline bool terminal_linefeed(Terminal *t) {
    Screen *s = t->active;

    if (s->cursor.y == t->scroll_bot) {
        if (terminal_region_is_whole_screen(t)) {
            return screen_cursor_down_scroll(s);
        }
        if (!screen_scroll_region_up(s, t->scroll_top, t->scroll_bot, 1)) {
            return false;
        }
        return screen_cursor_absolute(s, s->cursor.x, s->cursor.y);
    }

    if (s->cursor.y + 1 >= t->rows) return true;
    screen_cursor_down(s, 1);
    return true;
}

/* RI — the same in reverse, at the top of the region. */
inline bool terminal_reverse_index(Terminal *t) {
    Screen *s = t->active;

    if (s->cursor.y != t->scroll_top) {
        if (s->cursor.y > 0) screen_cursor_up(s, 1);
        return true;
    }

    /* Unlike a line feed there is no scrollback case: the row falling off the
     * bottom of the region never left the top of the screen, so it was never
     * history. */
    if (!screen_scroll_region_down(s, t->scroll_top, t->scroll_bot, 1)) {
        return false;
    }
    return screen_cursor_absolute(s, s->cursor.x, s->cursor.y);
}

/* ─── inserting and deleting lines ───────────────────────────────────────── */

/* IL — open n blank lines here, pushing the rest of the region down.
 *
 * A cursor outside the region does nothing at all, which is the standard and
 * is what stops a program that has left the cursor somewhere else from
 * disturbing a display it is not addressing. */
inline bool terminal_insert_lines(Terminal *t, CellCountInt n) {
    Screen *s = t->active;
    if (n == 0) n = 1;
    if (s->cursor.y < t->scroll_top || s->cursor.y > t->scroll_bot) return true;

    /* The region being scrolled is from the cursor down, not the whole one:
     * what is above the cursor stays where it is. */
    if (!screen_scroll_region_down(s, s->cursor.y, t->scroll_bot, n)) {
        return false;
    }

    /* Both of these leave the cursor at the left margin, which is in the
     * standard and which programs rely on. */
    return screen_cursor_absolute(s, 0, s->cursor.y);
}

/* DL — remove n lines here, pulling the rest of the region up. */
inline bool terminal_delete_lines(Terminal *t, CellCountInt n) {
    Screen *s = t->active;
    if (n == 0) n = 1;
    if (s->cursor.y < t->scroll_top || s->cursor.y > t->scroll_bot) return true;

    if (!screen_scroll_region_up(s, s->cursor.y, t->scroll_bot, n)) {
        return false;
    }
    return screen_cursor_absolute(s, 0, s->cursor.y);
}

/* ─── inserting and deleting characters ──────────────────────────────────── */

/* ICH — open n blank cells at the cursor, pushing the rest of the line right.
 * What falls off the end is gone. */
inline bool terminal_insert_chars(Terminal *t, CellCountInt n) {
    Screen *s = t->active;
    if (n == 0) n = 1;

    const CellCountInt x = s->cursor.x;
    if ((size_t)x + n >= (size_t)t->cols) {
        return screen_clear_cells(s, s->cursor.y, x,
                                  (CellCountInt)(t->cols - x), false);
    }

    Pin p = page_list_active_pin(&s->pages, 0, s->cursor.y);
    if (!p.valid()) return false;
    Page *page = &p.node->page;

    for (size_t i = t->cols; i-- > (size_t)x + n;) {
        page_erase_cell(page, (CellCountInt)i, p.y);
        if (!page_clone_cell(page, (CellCountInt)i, p.y, page,
                             (CellCountInt)(i - n), p.y)) {
            return false;
        }
    }

    const bool ok = screen_clear_cells(s, s->cursor.y, x, n, false);
    screen_cursor_reload(s);
    return ok;
}

/* DCH — remove n cells at the cursor, pulling the rest of the line left and
 * blanking what is vacated at the right. */
inline bool terminal_delete_chars(Terminal *t, CellCountInt n) {
    Screen *s = t->active;
    if (n == 0) n = 1;

    const CellCountInt x = s->cursor.x;
    if ((size_t)x + n >= (size_t)t->cols) {
        return screen_clear_cells(s, s->cursor.y, x,
                                  (CellCountInt)(t->cols - x), false);
    }

    Pin p = page_list_active_pin(&s->pages, 0, s->cursor.y);
    if (!p.valid()) return false;
    Page *page = &p.node->page;

    for (size_t i = x; i + n < (size_t)t->cols; i++) {
        page_erase_cell(page, (CellCountInt)i, p.y);
        if (!page_clone_cell(page, (CellCountInt)i, p.y, page,
                             (CellCountInt)(i + n), p.y)) {
            return false;
        }
    }

    const bool ok = screen_clear_cells(s, s->cursor.y,
                                       (CellCountInt)(t->cols - n), n, false);
    screen_cursor_reload(s);
    return ok;
}

/* ─── resizing ───────────────────────────────────────────────────────────── */

/* Resize both screens.
 *
 * The one that is not showing is resized too. A program on the alternate
 * screen expects the shell underneath to have noticed the new size by the
 * time it exits, and a primary screen left at the old width would reflow on
 * the way back at exactly the moment the user is looking at it. */
inline bool terminal_resize(Terminal *t, CellCountInt cols, CellCountInt rows) {
    if (cols == 0 || rows == 0) return false;

    if (!screen_resize(&t->primary, cols, rows)) return false;
    if (!screen_resize(&t->alternate, cols, rows)) return false;

    if (cols != t->cols) {
        uint8_t *tabs = (uint8_t *)malloc(cols);
        if (!tabs) return false;
        free(t->tabs);
        t->tabs = tabs;
        t->cols = cols;
        /* The stops go back to their defaults rather than being carried
         * across. A stop is a column number, and the columns have changed. */
        terminal_reset_tabs(t);
    }

    t->cols = cols;
    t->rows = rows;

    /* The region goes back to the whole screen. Its bounds are row numbers,
     * and a region that referred to rows the screen no longer has would leave
     * a program scrolling something it cannot see. */
    t->scroll_top = 0;
    t->scroll_bot = (CellCountInt)(rows - 1);
    return true;
}

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_TERMINAL_HPP */
