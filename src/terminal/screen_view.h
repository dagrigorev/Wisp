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

/* One image to draw this frame.
 *
 * Original to Wisp, not ported: the shape follows Ghostty's
 * renderer/image.zig `Placement`, but the fields are what Direct2D's
 * DrawBitmap needs rather than what a GPU vertex wants. The pixels are
 * borrowed from the terminal's ImageStorage and are valid only until the
 * next feed/sync, like every other pointer in this snapshot.
 *
 * Destination geometry is deliberately split into a cell position and a
 * pixel size: cell metrics are whole pixels and the painter's origin is
 * floored, so the renderer can land the image on exactly the same integer
 * grid the glyphs use. A `dest_col`/`dest_row` may be negative when a
 * placement is anchored above or left of the viewport and only partly
 * visible; the renderer clips rather than dropping it. */
typedef struct {
    /* The terminal's image ID. The renderer's bitmap cache is keyed on it. */
    uint32_t image_id;

    /* ImageStorage's per-image generation stamp. Strictly increasing and
     * unique; it changes whenever the image's pixels may have changed
     * (retransmission, format conversion, animation frame advance), and
     * does NOT change for a mere move. The renderer re-uploads exactly
     * when this differs from the cached value. */
    uint64_t generation;

    /* RGBA8888, premultiplied, 4 * pixel_width * pixel_height bytes. */
    const unsigned char *pixels;
    uint32_t pixel_width, pixel_height;

    /* Destination: top-left cell in viewport coordinates (may be negative
     * or past the grid; clip), plus the placement's pixel offset inside
     * that cell, plus the drawn size in pixels. */
    int32_t  dest_col, dest_row;
    uint32_t cell_offset_x, cell_offset_y;
    uint32_t dest_width_px, dest_height_px;

    /* Source sub-rectangle within the image, already intersected with the
     * image bounds. Kitty placements may show only part of an image. */
    uint32_t source_x, source_y, source_width, source_height;

    /* Kitty z-index. Negative draws behind text, non-negative in front. */
    int32_t z;
} ScreenImagePlacement;

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

    /* ── Images ──────────────────────────────────────────────────────────
     *
     * The placements to draw this frame, sorted by z and then by image ID
     * so the renderer can walk contiguous ranges rather than sort per
     * frame. `images_text_end` is the index of the first placement with
     * z >= 0: everything before it draws behind the glyphs, everything
     * from it on draws in front. `images_bg_end` splits off Kitty's
     * "below the cell backgrounds" tier (z < -2^30), which upstream's
     * renderer/image.zig also keeps as a separate pass. */
    ScreenImagePlacement *images;
    int images_len, images_cap;
    int images_bg_end;
    int images_text_end;

    /* Every image ID currently held by the terminal's storage, so the
     * renderer can release the bitmaps of images that are gone. This is
     * not the same as the placement list: an image scrolled out of the
     * viewport publishes no placement but must keep its bitmap. */
    uint32_t *image_ids;
    int image_ids_len, image_ids_cap;

    /* Bumped whenever the published placement list changes in any way.
     * Lets a caller tell "images moved" from "images are the same". */
    uint64_t images_serial;

    /* Internal to screen_view_images_end: the hash of the placement list
     * it last published, so it can tell a changed list from an identical
     * one without keeping a second copy of the array. */
    uint64_t images_hash;
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

/* ── Image publication, called by VtPane::sync ───────────────────────────
 *
 * A frame begins with screen_view_images_begin, appends placements in any
 * order, then calls screen_view_images_end, which sorts them by (z, image
 * id), computes the z-pass boundaries, and bumps `images_serial` if the
 * result differs from the previous frame's. The append functions return
 * false only on allocation failure, in which case the placement is simply
 * not published. */
void screen_view_images_begin(ScreenBuffer *sb);
bool screen_view_images_push(ScreenBuffer *sb, const ScreenImagePlacement *p);
bool screen_view_images_push_id(ScreenBuffer *sb, uint32_t image_id);
void screen_view_images_end(ScreenBuffer *sb);

/* True if `image_id` is still held by the terminal's image storage. The
 * renderer uses it to decide which cached bitmaps to release. */
bool screen_view_has_image(const ScreenBuffer *sb, uint32_t image_id);

#ifdef __cplusplus
}
#endif

#endif /* WISP_SCREEN_VIEW_H */
