#pragma once
#ifndef WISP_SCREEN_VIEW_H
#define WISP_SCREEN_VIEW_H

/* The renderer's view of a terminal.
 *
 * The terminal state itself lives in src/vt (the Ghostty port). This is the
 * flat, per-frame snapshot the Direct2D renderer walks: one cell per visible
 * grid position, plus the handful of terminal facts the window and renderer
 * need (cursor, title, scrollback depth, viewport offset).
 *
 * Cells hold an interned style ID rather than attributes: most cells on a
 * screen share a handful of styles, so the distinct ones live once in the
 * view's StyleTable and a cell keeps a two-byte ID. Style ID 0 is the default
 * style, so a zeroed cell is already sensible.
 *
 * This replaced screen.h/screen.c, which was a second terminal implementation
 * maintained alongside the port. Nothing here interprets escape sequences;
 * vt_pane fills it from vt::Terminal after each feed. */

#include <windows.h>
#include "wisp_bool.h"
#include <stdint.h>

#include "style_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t ch;           /* Unicode codepoint (0 = empty / space) */
    uint16_t style_id;     /* Index into the view's StyleTable; 0 = default */
    uint8_t  wide     : 1; /* CJK double-width lead cell */
    uint8_t  wide_cont: 1; /* CJK double-width continuation cell */
    uint8_t  dirty    : 1; /* Needs repaint */
    uint8_t  _pad     : 5;
} ScreenCell;

/* Default "blank" cell */
static inline ScreenCell screen_cell_blank(void) {
    ScreenCell c;
    c.ch = ' ';
    c.style_id = 0;
    c.wide = 0;
    c.wide_cont = 0;
    c.dirty = 1;
    c._pad = 0;
    return c;
}

typedef struct {
    /* The visible grid, [rows * cols], top-left first. This is the viewport,
     * so scrolled-back rows appear here in place of the active ones. */
    ScreenCell *cells;
    int         cols, rows;

    /* Interned styles referenced by every cell's style_id.
     *
     * Grow-only: a snapshot is rebuilt each frame, and releasing per cell
     * would mean reference traffic on every visible cell every frame. The
     * table tracks how many distinct styles a session uses, not how many
     * cells use them. */
    StyleTable *styles;

    /* Cursor position within the visible grid, and whether to draw it. */
    int  cursor_x, cursor_y;
    bool cursor_visible;

    /* Terminal facts the window acts on. */
    bool alt_screen_active;
    bool bracketed_paste;  /* mode 2004 */
    bool app_cursor_keys;  /* mode 1 (DECCKM) */

    /* Scrollback depth in rows and how far up the viewport is scrolled
     * (0 = live bottom). */
    int scrollback_count;
    int viewport_offset;

    /* Window title (OSC 0/2), empty when the program set none. */
    char title[256];
} ScreenBuffer;

/* Allocate the grid and style table. Returns false on allocation failure. */
bool screen_view_init(ScreenBuffer *sb, int cols, int rows);
void screen_view_free(ScreenBuffer *sb);

/* Reallocate the grid. Contents are not preserved; the next sync refills it. */
bool screen_view_resize(ScreenBuffer *sb, int cols, int rows);

/* The cell at a visible position, or NULL if out of bounds. */
const ScreenCell *screen_visible_cell(const ScreenBuffer *sb, int viewport_row, int col);

/* The largest viewport_offset the scrollback allows. */
int screen_max_viewport_offset(const ScreenBuffer *sb);

#ifdef __cplusplus
}
#endif

#endif /* WISP_SCREEN_VIEW_H */
