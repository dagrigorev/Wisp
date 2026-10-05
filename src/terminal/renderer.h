#pragma once
#ifndef WISP_RENDERER_H
#define WISP_RENDERER_H

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include "wisp_bool.h"
#include "screen_view.h"
#include "../platform/config.h"
#include "font.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ─── ANSI color palette (256 colors) ───────────────────────────────────── */

typedef struct {
    float r, g, b, a;
} Color4F;

/* ─── Renderer ───────────────────────────────────────────────────────────── */

typedef struct {
    /* Direct2D */
    ID2D1Factory          *d2d_factory;
    ID2D1HwndRenderTarget *render_target;
    ID2D1SolidColorBrush  *fg_brush;
    ID2D1SolidColorBrush  *bg_brush;
    ID2D1SolidColorBrush  *cursor_brush;

    /* DirectWrite via FontState */
    FontState              font;

    /* Layout dimensions */
    int    cols, rows;
    float  cell_w, cell_h;
    int    padding_x, padding_y;  /* pixels of padding around the grid */
    int    reserved_left_px;
    int    reserved_top_px;
    int    reserved_right_px;
    int    reserved_bottom_px;

    /* Color palette: 0-15 ANSI, 16-231 color cube, 232-255 grayscale */
    Color4F palette[256];
    Color4F bg_color;
    Color4F fg_color;
    Color4F cursor_color;
    Color4F selection_color;

    /* Text selection (cell coordinates) */
    int  sel_start_col, sel_start_row;
    int  sel_end_col,   sel_end_row;
    bool sel_active;    /* mouse button held */
    bool sel_valid;     /* a non-empty selection exists */

    /* Cursor state */
    bool   cursor_visible;
    bool   cursor_blink_state; /* true = drawn */
    int    cursor_style;       /* 0=block 1=bar 2=underline */
    UINT_PTR blink_timer_id;

    char   search_query[128];
    bool   search_active;

    /* Reasoning overlay (proactive AI subtitles) */

    /* DPI */
    float  dpi;

    /* Configuration reference */
    const Config *cfg;

    /* Style table of the buffer currently being painted. Cells hold style
       IDs rather than attributes, so drawing needs the table to resolve
       them. Set at the top of each paint from the ScreenBuffer. */
    const StyleTable *styles;

    /* Tab bar height (0 if tabs disabled) */
    int    tab_bar_height;

    HWND   hwnd;
} Renderer;

/* ─── API ────────────────────────────────────────────────────────────────── */

bool renderer_init(Renderer *r, HWND hwnd, const Config *cfg);
void renderer_resize(Renderer *r, int width_px, int height_px);
void renderer_paint(Renderer *r, const ScreenBuffer *sb,
                    bool cursor_at_x, int cursor_x, int cursor_y);
void renderer_begin_frame(Renderer *r);
void renderer_paint_region(Renderer *r, const ScreenBuffer *sb, const RECT *rect,
                           bool active, bool cursor_shown, int cursor_x, int cursor_y);
void renderer_end_frame(Renderer *r);
void renderer_set_font(Renderer *r, const wchar_t *family, float pt_size);
void renderer_update_dpi(Renderer *r, float dpi);
void renderer_toggle_cursor_blink(Renderer *r);
void renderer_pixel_to_cell(const Renderer *r, int px, int py, int *col, int *row);
void renderer_destroy(Renderer *r);

/* Map a 24-bit RGB color to Color4F */
Color4F renderer_rgb_to_color(uint32_t rgb);

/* Resolve a cell's fg/bg to Color4F (handles 256-color and truecolor) */
Color4F renderer_resolve_fg(const Renderer *r, const ScreenCell *cell);
Color4F renderer_resolve_bg(const Renderer *r, const ScreenCell *cell);


#ifdef __cplusplus
}
#endif

#endif /* WISP_RENDERER_H */
