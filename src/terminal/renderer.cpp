/* renderer.cpp — Direct2D + DirectWrite terminal cell renderer
 * C++ COM syntax: obj->Method() not obj->lpVtbl->Method()
 */
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "renderer.h"
#include "screen_view.h"
#include "../platform/config.h"
#include "font.h"
#include "layout.h"
#include "../core/str_util.h"
#include "../core/log.h"

static D2D1_COLOR_F to_d2d(Color4F c) { D2D1_COLOR_F d={c.r,c.g,c.b,c.a}; return d; }

Color4F renderer_rgb_to_color(uint32_t rgb) {
    Color4F c; c.r=((rgb>>16)&0xFF)/255.0f; c.g=((rgb>>8)&0xFF)/255.0f;
    c.b=(rgb&0xFF)/255.0f; c.a=1.0f; return c;
}

static void build_palette(Renderer *r, const Config *cfg) {
    for (int i=0;i<16;i++) r->palette[i]=renderer_rgb_to_color(cfg->colors.ansi[i]);
    for (int i=16;i<232;i++) {
        int n=i-16, b=n%6; n/=6; int g=n%6; n/=6; int rd=n%6;
        r->palette[i].r=rd?(55+rd*40)/255.0f:0; r->palette[i].g=g?(55+g*40)/255.0f:0;
        r->palette[i].b=b?(55+b*40)/255.0f:0; r->palette[i].a=1.0f;
    }
    for (int i=232;i<256;i++) {
        float v=(8+(i-232)*10)/255.0f;
        r->palette[i].r=r->palette[i].g=r->palette[i].b=v; r->palette[i].a=1.0f;
    }
    r->bg_color=renderer_rgb_to_color(cfg->colors.background);
    r->fg_color=renderer_rgb_to_color(cfg->colors.foreground);
    r->cursor_color=renderer_rgb_to_color(cfg->colors.cursor);
    r->selection_color=renderer_rgb_to_color(cfg->colors.selection);
}

/* Cells store a style ID, so drawing resolves it through the buffer's table.
   The attr-taking forms exist so a paint loop can resolve once per cell
   rather than once per field it reads. */
static Color4F resolve_fg_attr(const Renderer *r, const CellAttr &a) {
    if (a.fg_idx==0xFF) return renderer_rgb_to_color(a.fg_rgb);
    if (a.dim) { Color4F c=r->palette[a.fg_idx]; c.r*=.5f;c.g*=.5f;c.b*=.5f; return c; }
    return r->palette[a.fg_idx<256?a.fg_idx:7];
}
static Color4F resolve_bg_attr(const Renderer *r, const CellAttr &a) {
    if (a.bg_idx==0xFF) return renderer_rgb_to_color(a.bg_rgb);
    if (a.bg_idx==0) return r->bg_color;
    return r->palette[a.bg_idx<256?a.bg_idx:0];
}

static CellAttr cell_attr(const Renderer *r, const ScreenCell *cell) {
    return style_table_resolve(r->styles, cell->style_id);
}

Color4F renderer_resolve_fg(const Renderer *r, const ScreenCell *cell) {
    return resolve_fg_attr(r, cell_attr(r, cell));
}
Color4F renderer_resolve_bg(const Renderer *r, const ScreenCell *cell) {
    return resolve_bg_attr(r, cell_attr(r, cell));
}

/* ─── Images ──────────────────────────────────────────────────────────────
 *
 * Original to Wisp, not a transliteration. Ghostty's renderer/image.zig is
 * written against OpenGL and Metal texture objects and a batched quad
 * pipeline; Direct2D has neither, so only the structure is borrowed:
 *
 *   - one GPU resource per image ID, not per placement;
 *   - re-upload keyed on the terminal's monotonic per-image `generation`,
 *     so a placement that merely moved costs nothing (upstream keeps the
 *     same stamp beside its texture in `ImageMap`);
 *   - a resource is released when its image leaves the terminal's
 *     storage, which upstream spells `markForUnload` + a sweep in
 *     `State.upload`;
 *   - drawing walks the z-sorted placement list in ranges, with the
 *     glyph pass between the z<0 and z>=0 ranges, which is upstream's
 *     `kitty_bg_end` / `kitty_text_end` split.
 *
 * Everything else — the BGRA swizzle, the integer destination rect, the
 * clip, the device-loss handling — is Direct2D's own problem.
 *
 * The snapshot hands us RGBA8888. An ID2D1HwndRenderTarget is not required
 * to accept R8G8B8A8, and B8G8R8A8 is the format that is universally
 * supported, so the bytes are swizzled into a scratch buffer on upload.
 * That cost is paid once per generation change, never per frame. */

struct ImageCacheEntry {
    uint32_t     image_id;
    uint64_t     generation;
    ID2D1Bitmap *bitmap;
};

struct ImageCache {
    ImageCacheEntry *entries;
    int              len, cap;

    /* Scratch for the RGBA -> BGRA swizzle, grown to the largest image
       uploaded so far and reused. */
    unsigned char   *scratch;
    size_t           scratch_cap;
};

static void image_cache_release_all(Renderer *r) {
    if (!r->images) return;
    for (int i = 0; i < r->images->len; ++i) {
        if (r->images->entries[i].bitmap) r->images->entries[i].bitmap->Release();
    }
    r->images->len = 0;
}

static void image_cache_destroy(Renderer *r) {
    if (!r->images) return;
    image_cache_release_all(r);
    free(r->images->entries);
    free(r->images->scratch);
    free(r->images);
    r->images = NULL;
}

static ImageCache *image_cache_get(Renderer *r) {
    if (!r->images) r->images = (ImageCache *)calloc(1, sizeof(ImageCache));
    return r->images;
}

static void image_cache_remove_at(ImageCache *c, int i) {
    if (c->entries[i].bitmap) c->entries[i].bitmap->Release();
    c->entries[i] = c->entries[c->len - 1];
    c->len--;
}

/* Drop every cached bitmap whose image is no longer in the snapshot's
   live-ID list. Called once per painted buffer, before drawing. */
static void image_cache_evict(Renderer *r, const ScreenBuffer *sb) {
    ImageCache *c = r->images;
    if (!c) return;
    /* An empty list is only authoritative when the snapshot actually
       published one; a buffer that never carried images leaves the cache
       alone rather than thrashing it. */
    for (int i = c->len - 1; i >= 0; --i) {
        if (!screen_view_has_image(sb, c->entries[i].image_id)) image_cache_remove_at(c, i);
    }
}

static ID2D1Bitmap *image_upload(Renderer *r, const ScreenImagePlacement *p) {
    ImageCache *c = image_cache_get(r);
    if (!c || !r->render_target) return NULL;

    for (int i = 0; i < c->len; ++i) {
        if (c->entries[i].image_id != p->image_id) continue;
        if (c->entries[i].generation == p->generation) return c->entries[i].bitmap;
        /* Same ID, different pixels: the old bitmap is stale. */
        image_cache_remove_at(c, i);
        break;
    }

    if (!p->pixels || !p->pixel_width || !p->pixel_height) return NULL;

    const size_t px = (size_t)p->pixel_width * (size_t)p->pixel_height;
    const size_t bytes = px * 4;
    if (bytes / 4 != px) return NULL; /* overflow on a protocol-sized image */

    if (c->scratch_cap < bytes) {
        unsigned char *grown = (unsigned char *)realloc(c->scratch, bytes);
        if (!grown) return NULL;
        c->scratch = grown;
        c->scratch_cap = bytes;
    }
    for (size_t i = 0; i < px; ++i) {
        const unsigned char *s = p->pixels + i * 4;
        unsigned char *d = c->scratch + i * 4;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
    }

    D2D1_BITMAP_PROPERTIES props;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    /* The pixel pipeline in terminal/kitty/graphics_pixel.hpp produces
       premultiplied alpha (upstream asks wuffs for RGBA_PREMUL), so this
       must not be STRAIGHT or every translucent image darkens. */
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = props.dpiY = 96.0f;

    D2D1_SIZE_U size = {(UINT32)p->pixel_width, (UINT32)p->pixel_height};
    ID2D1Bitmap *bmp = NULL;
    HRESULT hr = r->render_target->CreateBitmap(size, c->scratch,
                                                (UINT32)(p->pixel_width * 4), props, &bmp);
    if (FAILED(hr) || !bmp) {
        WISP_LOG_ERROR("CreateBitmap for image %u: 0x%08X", p->image_id, hr);
        return NULL;
    }

    if (c->len == c->cap) {
        const int cap = c->cap ? c->cap * 2 : 8;
        ImageCacheEntry *grown =
            (ImageCacheEntry *)realloc(c->entries, (size_t)cap * sizeof(*grown));
        if (!grown) { bmp->Release(); return NULL; }
        c->entries = grown;
        c->cap = cap;
    }
    c->entries[c->len].image_id = p->image_id;
    c->entries[c->len].generation = p->generation;
    c->entries[c->len].bitmap = bmp;
    c->len++;
    return bmp;
}

/* Draw placements [lo, hi) of the snapshot's z-sorted list.
 *
 * `ox`/`oy` are the painter's grid origin, already floored, and cell_w /
 * cell_h are integral, so every edge computed here lands on the same whole
 * pixel a glyph would. Doing this in float would put images on their own
 * subpixel phase and make them shimmer against text that does not. */
static void draw_images(Renderer *r, const ScreenBuffer *sb, int lo, int hi,
                        float ox, float oy, int grid_cols, int grid_rows) {
    if (!r->render_target || lo >= hi || grid_cols <= 0 || grid_rows <= 0) return;

    const int cw = (int)r->cell_w;
    const int ch = (int)r->cell_h;
    if (cw <= 0 || ch <= 0) return;

    const int gx = (int)ox;
    const int gy = (int)oy;

    /* Images may be larger than the pane, and a partly visible placement
       is published with its real off-grid position, so the grid rectangle
       is the clip. Without it Direct2D would happily paint over the
       padding and the scrollbar. */
    D2D1_RECT_F grid = {(float)gx, (float)gy, (float)(gx + grid_cols * cw),
                        (float)(gy + grid_rows * ch)};
    r->render_target->PushAxisAlignedClip(grid, D2D1_ANTIALIAS_MODE_ALIASED);

    for (int i = lo; i < hi && i < sb->images_len; ++i) {
        const ScreenImagePlacement *p = &sb->images[i];
        ID2D1Bitmap *bmp = image_upload(r, p);
        if (!bmp) continue;

        const int x0 = gx + p->dest_col * cw + (int)p->cell_offset_x;
        const int y0 = gy + p->dest_row * ch + (int)p->cell_offset_y;
        D2D1_RECT_F dest = {(float)x0, (float)y0, (float)(x0 + (int)p->dest_width_px),
                            (float)(y0 + (int)p->dest_height_px)};
        D2D1_RECT_F src = {(float)p->source_x, (float)p->source_y,
                           (float)(p->source_x + p->source_width),
                           (float)(p->source_y + p->source_height)};

        r->render_target->DrawBitmap(bmp, &dest, 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    }

    r->render_target->PopAxisAlignedClip();
}

static bool create_rt(Renderer *r) {
    /* Bitmaps are created from the render target, so they die with it.
       This is the device-loss path (D2DERR_RECREATE_TARGET) as well as the
       DPI-change path; either way every cached image must go, and the next
       paint re-uploads from the snapshot's still-live pixels. */
    image_cache_release_all(r);
    if (r->render_target) { r->render_target->Release(); r->render_target=NULL; }
    RECT rc; GetClientRect(r->hwnd,&rc);
    D2D1_SIZE_U sz={(UINT32)(rc.right-rc.left),(UINT32)(rc.bottom-rc.top)};
    if (!sz.width) sz.width=1; if (!sz.height) sz.height=1;

    D2D1_RENDER_TARGET_PROPERTIES rp;
    rp.type=D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rp.pixelFormat.format=DXGI_FORMAT_UNKNOWN; rp.pixelFormat.alphaMode=D2D1_ALPHA_MODE_UNKNOWN;
    rp.dpiX=rp.dpiY=0; rp.usage=D2D1_RENDER_TARGET_USAGE_NONE; rp.minLevel=D2D1_FEATURE_LEVEL_DEFAULT;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hp; hp.hwnd=r->hwnd; hp.pixelSize=sz; hp.presentOptions=D2D1_PRESENT_OPTIONS_NONE;

    HRESULT hr=r->d2d_factory->CreateHwndRenderTarget(rp,hp,&r->render_target);
    if (FAILED(hr)) { WISP_LOG_ERROR("CreateHwndRenderTarget: 0x%08X",hr); return false; }

    if (r->fg_brush)     { r->fg_brush->Release();     r->fg_brush=NULL; }
    if (r->bg_brush)     { r->bg_brush->Release();     r->bg_brush=NULL; }
    if (r->cursor_brush) { r->cursor_brush->Release(); r->cursor_brush=NULL; }

    D2D1_COLOR_F col=to_d2d(r->fg_color); r->render_target->CreateSolidColorBrush(col,&r->fg_brush);
    col=to_d2d(r->bg_color); r->render_target->CreateSolidColorBrush(col,&r->bg_brush);
    col=to_d2d(r->cursor_color); r->render_target->CreateSolidColorBrush(col,&r->cursor_brush);
    return true;
}

bool renderer_init(Renderer *r, HWND hwnd, const Config *cfg) {
    memset(r,0,sizeof(*r)); r->hwnd=hwnd; r->cfg=cfg; r->padding_x=8; r->padding_y=8;
    HDC hdc=GetDC(hwnd); r->dpi=hdc?(float)GetDeviceCaps(hdc,LOGPIXELSX):96.0f; if(hdc)ReleaseDC(hwnd,hdc);
    build_palette(r, cfg);
    D2D1_FACTORY_OPTIONS opts={D2D1_DEBUG_LEVEL_NONE};
    HRESULT hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,opts,&r->d2d_factory);
    if (FAILED(hr)) { WISP_LOG_ERROR("D2D1CreateFactory: 0x%08X",hr); return false; }
    if (!create_rt(r)) return false;
    if (!font_init(&r->font,cfg->font.family,cfg->font.size,r->dpi)) { WISP_LOG_ERROR("font_init failed"); return false; }
    r->cell_w=r->font.cell_width; r->cell_h=r->font.cell_height;
    r->cursor_style=cfg->cursor.style; r->cursor_visible=true; r->cursor_blink_state=true;
    r->tab_bar_height=0;
    if (cfg->cursor.blink) r->blink_timer_id=SetTimer(hwnd,1,cfg->cursor.blink_rate_ms,NULL);
    RECT rc; GetClientRect(hwnd,&rc); renderer_resize(r,rc.right-rc.left,rc.bottom-rc.top);
    return true;
}

void renderer_resize(Renderer *r, int w, int h) {
    if (w<1)w=1; if(h <1)h=1;
    if (r->render_target) { D2D1_SIZE_U sz={(UINT32)w,(UINT32)h}; r->render_target->Resize(sz); }
    TerminalGridLayout layout = terminal_compute_grid_layout_ex(w, h, r->cell_w, r->cell_h,
                                                                r->padding_x, r->padding_y,
                                                                r->reserved_left_px,
                                                                r->reserved_top_px,
                                                                r->reserved_right_px,
                                                                r->reserved_bottom_px);
    r->cols = layout.cols;
    r->rows = layout.rows;
}


void renderer_grid_size(const Renderer *r, int width_px, int height_px, int *cols, int *rows) {
    const TerminalGridLayout layout =
        terminal_compute_grid_layout_ex(width_px, height_px, r->cell_w, r->cell_h,
                                        r->padding_x, r->padding_y, 0, 0, 0, 0);
    if (cols) *cols = layout.cols > 0 ? layout.cols : 1;
    if (rows) *rows = layout.rows > 0 ? layout.rows : 1;
}


/* Convert window pixel coords to cell col/row (-1 if outside grid) */
void renderer_pixel_to_cell(const Renderer *r, int px, int py, int *col, int *row) {
    TerminalGridLayout layout = {0};
    layout.cols = r->cols;
    layout.rows = r->rows;
    layout.origin_x_px = r->reserved_left_px + r->padding_x;
    layout.origin_y_px = r->reserved_top_px + r->padding_y;
    terminal_pixel_to_cell_ex(&layout, px, py, r->cell_w, r->cell_h,
                              r->padding_x, r->padding_y, col, row);
}


void renderer_begin_frame(Renderer *r) {
    if (!r->render_target) return;
    r->render_target->BeginDraw();
    D2D1_COLOR_F bgc = to_d2d(r->bg_color);
    r->render_target->Clear(&bgc);
}

void renderer_end_frame(Renderer *r) {
    if (!r->render_target) return;
    HRESULT hr = r->render_target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) create_rt(r);
}

static void renderer_draw_pane_border(Renderer *r, const RECT *rect, bool active) {
    if (!r || !r->render_target || !rect) return;
    D2D1_COLOR_F c = active ? to_d2d(r->cursor_color) : to_d2d(r->fg_color);
    c.a = active ? 0.75f : 0.25f;
    r->fg_brush->SetColor(c);
    D2D1_RECT_F rr = {(float)rect->left + 0.5f, (float)rect->top + 0.5f,
                      (float)rect->right - 0.5f, (float)rect->bottom - 0.5f};
    r->render_target->DrawRectangle(rr, r->fg_brush, active ? 2.0f : 1.0f);
}

void renderer_paint_region(Renderer *r, const ScreenBuffer *sb, const RECT *rect,
                           bool active, bool cursor_shown, int cursor_x, int cursor_y) {
    if (!r || !r->render_target || !r->font.fmt_normal || !sb || !rect) return;

    /* Cells hold style IDs, so drawing needs this buffer's table to resolve
       them. Panes each have their own buffer, so this is set per paint
       rather than once at init. */
    r->styles = sb->styles;

    float pane_w = (float)(rect->right - rect->left);
    float pane_h = (float)(rect->bottom - rect->top);
    if (pane_w <= 1.0f || pane_h <= 1.0f) return;

    D2D1_RECT_F clip = {(float)rect->left, (float)rect->top, (float)rect->right, (float)rect->bottom};
    r->render_target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    D2D1_COLOR_F bgc = to_d2d(r->bg_color);
    r->bg_brush->SetColor(bgc);
    r->render_target->FillRectangle(clip, r->bg_brush);

    /* Images whose bitmaps outlived their terminal images go now, before
       any pass can draw one. */
    image_cache_evict(r, sb);

    /* The same grid the terminal was sized to. Computing it differently here
     * is how the bottom row stopped being painted. */
    int max_cols = 1, max_rows = 1;
    renderer_grid_size(r, (int)pane_w, (int)pane_h, &max_cols, &max_rows);

    int rows = sb->rows < max_rows ? sb->rows : max_rows;
    int cols = sb->cols < max_cols ? sb->cols : max_cols;
    /* Whole pixels: cell_w/cell_h are integral, so an integral origin keeps
     * every cell on the pixel grid. A fractional origin would put each column
     * on its own subpixel phase and bring back the uneven glyph spacing and
     * soft background edges that integral cell metrics are there to avoid. */
    float ox = floorf((float)rect->left + (float)r->padding_x);
    float oy = floorf((float)rect->top  + (float)r->padding_y);
    wchar_t wch[4];

    bool *search_mask = NULL;
    if (active && r->search_active && r->search_query[0] && cols > 0) {
        search_mask = (bool *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)cols * sizeof(bool));
    }

    /* Kitty's deepest tier: z below -2^30 draws under the cell
       backgrounds, so it goes before anything else. */
    draw_images(r, sb, 0, sb->images_bg_end, ox, oy, cols, rows);

    /* Two passes over the grid so the image layers can sit between them:
       pass 0 paints cell backgrounds, search highlight, selection and the
       block cursor; pass 1 paints glyphs, underlines and the thin cursors.
       Kitty puts negative-z images above the backgrounds but below the
       text, which is exactly the seam between the two, and non-negative z
       above both. This is upstream's kitty_bg_end / kitty_text_end split;
       a single interleaved loop cannot express it. */
    for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) draw_images(r, sb, sb->images_bg_end, sb->images_text_end, ox, oy, cols, rows);
    for (int row = 0; row < rows; row++) {
        if (pass == 0 && search_mask) {
            memset(search_mask, 0, (size_t)cols * sizeof(bool));
            char line[4096];
            int li = 0;
            for (int c = 0; c < cols && li < (int)sizeof(line) - 1; ++c) {
                const ScreenCell *cell = screen_visible_cell(sb, row, c);
                if (!cell) cell = &sb->cells[row * sb->cols + c];
                uint32_t ch = cell && cell->ch ? cell->ch : ' ';
                line[li++] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
            }
            line[li] = '\0';
            const char *pos = line;
            size_t qlen = strlen(r->search_query);
            while (qlen > 0 && (pos = strstr(pos, r->search_query)) != NULL) {
                int start = (int)(pos - line);
                for (int k = 0; k < (int)qlen && start + k < cols; ++k) search_mask[start + k] = true;
                pos += qlen;
            }
        }
        for (int col = 0; col < cols; col++) {
            const ScreenCell *cell = screen_visible_cell(sb, row, col);
            if (!cell) cell = &sb->cells[row * sb->cols + col];

            float cx = ox + col * r->cell_w;
            float cy = oy + row * r->cell_h;
            D2D1_RECT_F cr = {cx, cy, cx + r->cell_w, cy + r->cell_h};
            const CellAttr ca = cell_attr(r, cell);
            Color4F fg = resolve_fg_attr(r, ca), bgcc = resolve_bg_attr(r, ca);
            if (ca.reverse) { Color4F t = fg; fg = bgcc; bgcc = t; }

            if (pass == 0 &&
                (bgcc.r != r->bg_color.r || bgcc.g != r->bg_color.g || bgcc.b != r->bg_color.b)) {
                D2D1_COLOR_F d = to_d2d(bgcc); r->bg_brush->SetColor(d);
                r->render_target->FillRectangle(cr, r->bg_brush);
            }
            if (pass == 0 && search_mask && search_mask[col]) {
                D2D1_COLOR_F hc = to_d2d(r->cursor_color);
                hc.a = 0.32f;
                r->bg_brush->SetColor(hc);
                r->render_target->FillRectangle(cr, r->bg_brush);
            }
            bool in_sel = false;
            if (r->sel_valid) {
                int sr = r->sel_start_row, sc = r->sel_start_col;
                int er = r->sel_end_row,   ec = r->sel_end_col;
                if (sr > er || (sr == er && sc > ec)) { int tr=sr,tc=sc; sr=er;sc=ec; er=tr;ec=tc; }
                if (row > sr && row < er) in_sel = true;
                else if (row == sr && row == er) in_sel = (col >= sc && col <= ec);
                else if (row == sr) in_sel = (col >= sc);
                else if (row == er) in_sel = (col <= ec);
            }
            if (pass == 0 && in_sel) {
                D2D1_COLOR_F selc = to_d2d(r->selection_color);
                r->bg_brush->SetColor(selc);
                r->render_target->FillRectangle(cr, r->bg_brush);
            }
            bool cursor_here = cursor_shown && active && r->cursor_blink_state &&
                               col == cursor_x && row == cursor_y && sb->cursor_visible;
            if (pass == 0 && cursor_here && r->cursor_style == CURSOR_BLOCK) {
                D2D1_COLOR_F cc = to_d2d(r->cursor_color);
                r->cursor_brush->SetColor(cc);
                r->render_target->FillRectangle(cr, r->cursor_brush);
            }

            if (pass == 1 && !cell->wide_cont && cell->ch && cell->ch != ' ') {
                if (cell->ch < 0x10000) { wch[0] = (wchar_t)cell->ch; wch[1] = 0; }
                else { uint32_t cp = cell->ch - 0x10000; wch[0] = (wchar_t)(0xD800 | (cp >> 10)); wch[1] = (wchar_t)(0xDC00 | (cp & 0x3FF)); wch[2] = 0; }

                IDWriteTextFormat *fmt = r->font.fmt_normal;
                if (ca.bold && ca.italic) fmt = r->font.fmt_bold_italic;
                else if (ca.bold) fmt = r->font.fmt_bold;
                else if (ca.italic) fmt = r->font.fmt_italic;

                IDWriteTextLayout *layout = NULL;
                if (r->font.factory) r->font.factory->CreateTextLayout(wch, (UINT32)(cell->ch >= 0x10000 ? 2 : 1), fmt, r->cell_w * (cell->wide ? 2.0f : 1.0f), r->cell_h, &layout);
                if (layout) {
                    D2D1_POINT_2F orig = {cx, cy}; D2D1_COLOR_F d = to_d2d(fg); r->fg_brush->SetColor(d);
                    r->render_target->DrawTextLayout(orig, layout, r->fg_brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    layout->Release();
                }
                if (ca.underline) {
                    float uy = cy + r->cell_h - 2.0f;
                    D2D1_POINT_2F p1 = {cx, uy}, p2 = {cx + r->cell_w, uy}; D2D1_COLOR_F d = to_d2d(fg); r->fg_brush->SetColor(d);
                    r->render_target->DrawLine(p1, p2, r->fg_brush, 1.0f, NULL);
                }
            }

            if (pass == 1 && cursor_here && r->cursor_style != CURSOR_BLOCK) {
                D2D1_COLOR_F cc = to_d2d(r->cursor_color); r->cursor_brush->SetColor(cc);
                switch (r->cursor_style) {
                    case CURSOR_BLOCK: break;
                    case CURSOR_BAR: { D2D1_RECT_F b = {cx, cy, cx + 2.0f, cy + r->cell_h}; r->render_target->FillRectangle(b, r->cursor_brush); break; }
                    case CURSOR_UNDERLINE: { D2D1_RECT_F u = {cx, cy + r->cell_h - 2.0f, cx + r->cell_w, cy + r->cell_h}; r->render_target->FillRectangle(u, r->cursor_brush); break; }
                }
            }
        }
    }
    } /* pass */

    /* Non-negative z draws over the text. */
    draw_images(r, sb, sb->images_text_end, sb->images_len, ox, oy, cols, rows);

    if (search_mask) HeapFree(GetProcessHeap(), 0, search_mask);

    if (r->cfg && r->cfg->scrollbar.enabled && !sb->alt_screen_active && sb->scrollback_count > 0) {
        int total_lines = sb->scrollback_count + sb->rows;
        int max_offset = screen_max_viewport_offset(sb);
        float track_w = (float)(r->cfg->scrollbar.width_px > 0 ? r->cfg->scrollbar.width_px : 8);
        float grid_h = rows * r->cell_h;
        float track_x0 = (float)rect->right - (float)r->padding_x - track_w;
        float track_x1 = track_x0 + track_w;
        float track_y0 = oy;
        float track_y1 = oy + grid_h;
        if (track_x0 > ox && track_y1 > track_y0) {
            D2D1_COLOR_F track = to_d2d(r->bg_color);
            track.a = 0.35f;
            r->bg_brush->SetColor(track);
            D2D1_ROUNDED_RECT tr = {{track_x0, track_y0, track_x1, track_y1}, track_w * 0.5f, track_w * 0.5f};
            r->render_target->FillRoundedRectangle(tr, r->bg_brush);

            float thumb_h = grid_h * ((float)sb->rows / (float)total_lines);
            if (thumb_h < r->cell_h) thumb_h = r->cell_h;
            if (thumb_h > grid_h) thumb_h = grid_h;

            float travel = grid_h - thumb_h;
            float denom = (float)(max_offset > 0 ? max_offset : 1);
            float live_to_top = denom > 0 ? ((float)sb->viewport_offset / denom) : 0.0f;
            float thumb_y = track_y1 - thumb_h - travel * live_to_top;
            if (thumb_y < track_y0) thumb_y = track_y0;
            if (thumb_y + thumb_h > track_y1) thumb_y = track_y1 - thumb_h;

            D2D1_COLOR_F thumb = to_d2d(r->cursor_color);
            thumb.a = active ? 0.60f : 0.35f;
            r->fg_brush->SetColor(thumb);
            D2D1_ROUNDED_RECT th = {{track_x0, thumb_y, track_x1, thumb_y + thumb_h}, track_w * 0.5f, track_w * 0.5f};
            r->render_target->FillRoundedRectangle(th, r->fg_brush);
        }
    }

    r->render_target->PopAxisAlignedClip();
    renderer_draw_pane_border(r, rect, active);
}

void renderer_paint(Renderer *r, const ScreenBuffer *sb, bool cursor_shown, int cursor_x, int cursor_y) {
    if (!r->render_target||!r->font.fmt_normal) return;
    r->styles = sb->styles;
    r->render_target->BeginDraw();
    D2D1_COLOR_F bgc=to_d2d(r->bg_color); r->render_target->Clear(&bgc);

    int rows=sb->rows<r->rows?sb->rows:r->rows, cols=sb->cols<r->cols?sb->cols:r->cols;
    float ox=(float)(r->reserved_left_px + r->padding_x);
    float oy=(float)(r->reserved_top_px + r->padding_y);
    wchar_t wch[4];

    /* This whole-window painter is the older single-pane path; the app
       paints through renderer_paint_region. Images are drawn here so the
       two paths do not disagree about whether images exist at all, but the
       split is only two-way: everything below z 0 goes under the cells
       rather than between the background and the glyph, because this
       loop interleaves background and glyph per cell. */
    image_cache_evict(r, sb);
    draw_images(r, sb, 0, sb->images_text_end, ox, oy, cols, rows);

    for (int row=0;row<rows;row++) for (int col=0;col<cols;col++) {
        const ScreenCell *cell = screen_visible_cell(sb, row, col);
        if (!cell) cell = &sb->cells[row * sb->cols + col];

        float cx=ox+col*r->cell_w, cy=oy+row*r->cell_h;
        D2D1_RECT_F cr={cx,cy,cx+r->cell_w,cy+r->cell_h};
        const CellAttr ca = cell_attr(r, cell);
        Color4F fg=resolve_fg_attr(r,ca), bgcc=resolve_bg_attr(r,ca);
        if (ca.reverse) { Color4F t=fg; fg=bgcc; bgcc=t; }

        if (bgcc.r!=r->bg_color.r||bgcc.g!=r->bg_color.g||bgcc.b!=r->bg_color.b) {
            D2D1_COLOR_F d=to_d2d(bgcc); r->bg_brush->SetColor(d);
            r->render_target->FillRectangle(cr,r->bg_brush);
        }

        /* Selection highlight */
        bool in_sel = false;
        if (r->sel_valid) {
            int sr = r->sel_start_row, sc = r->sel_start_col;
            int er = r->sel_end_row,   ec = r->sel_end_col;
            /* Normalize start < end */
            if (sr > er || (sr == er && sc > ec)) {
                int tr=sr,tc=sc; sr=er;sc=ec; er=tr;ec=tc;
            }
            if (row > sr && row < er) in_sel = true;
            else if (row == sr && row == er) in_sel = (col >= sc && col <= ec);
            else if (row == sr) in_sel = (col >= sc);
            else if (row == er) in_sel = (col <= ec);
        }
        if (in_sel) {
            D2D1_COLOR_F selc = to_d2d(r->selection_color);
            r->bg_brush->SetColor(selc);
            r->render_target->FillRectangle(cr, r->bg_brush);
        }
        bool cursor_here = cursor_shown && r->cursor_blink_state &&
                           col == cursor_x && row == cursor_y && sb->cursor_visible;
        if (cursor_here && r->cursor_style == CURSOR_BLOCK) {
            D2D1_COLOR_F cc = to_d2d(r->cursor_color);
            r->cursor_brush->SetColor(cc);
            r->render_target->FillRectangle(cr, r->cursor_brush);
        }

        if (!cell->wide_cont&&cell->ch&&cell->ch!=' ') {
            if (cell->ch<0x10000) { wch[0]=(wchar_t)cell->ch; wch[1]=0; }
            else { uint32_t cp=cell->ch-0x10000; wch[0]=(wchar_t)(0xD800|(cp>>10)); wch[1]=(wchar_t)(0xDC00|(cp&0x3FF)); wch[2]=0; }

            IDWriteTextFormat *fmt=r->font.fmt_normal;
            if (ca.bold&&ca.italic) fmt=r->font.fmt_bold_italic;
            else if (ca.bold)       fmt=r->font.fmt_bold;
            else if (ca.italic)     fmt=r->font.fmt_italic;

            IDWriteTextLayout *layout=NULL;
            if (r->font.factory) r->font.factory->CreateTextLayout(wch,(UINT32)(cell->ch>=0x10000?2:1),fmt,r->cell_w*(cell->wide?2.0f:1.0f),r->cell_h,&layout);
            if (layout) {
                D2D1_POINT_2F orig={cx,cy}; D2D1_COLOR_F d=to_d2d(fg); r->fg_brush->SetColor(d);
                r->render_target->DrawTextLayout(orig,layout,r->fg_brush,D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                layout->Release();
            }
            if (ca.underline) {
                float uy=cy+r->cell_h-2.0f;
                D2D1_POINT_2F p1={cx,uy},p2={cx+r->cell_w,uy}; D2D1_COLOR_F d=to_d2d(fg); r->fg_brush->SetColor(d);
                r->render_target->DrawLine(p1,p2,r->fg_brush,1.0f,NULL);
            }
        }

        if (cursor_here && r->cursor_style != CURSOR_BLOCK) {
            D2D1_COLOR_F cc=to_d2d(r->cursor_color); r->cursor_brush->SetColor(cc);
            switch(r->cursor_style) {
                case CURSOR_BLOCK: break;
                case CURSOR_BAR: { D2D1_RECT_F b={cx,cy,cx+2.0f,cy+r->cell_h}; r->render_target->FillRectangle(b,r->cursor_brush); break; }
                case CURSOR_UNDERLINE: { D2D1_RECT_F u={cx,cy+r->cell_h-2.0f,cx+r->cell_w,cy+r->cell_h}; r->render_target->FillRectangle(u,r->cursor_brush); break; }
            }
        }
    }

    draw_images(r, sb, sb->images_text_end, sb->images_len, ox, oy, cols, rows);

    if (r->cfg && r->cfg->scrollbar.enabled && !sb->alt_screen_active && sb->scrollback_count > 0) {
        int total_lines = sb->scrollback_count + sb->rows;
        int max_offset = screen_max_viewport_offset(sb);
        int view_start = total_lines - sb->rows - sb->viewport_offset;
        if (view_start < 0) view_start = 0;

        float track_w = (float)(r->cfg->scrollbar.width_px > 0 ? r->cfg->scrollbar.width_px : 8);
        float grid_h = rows * r->cell_h;
        float track_x0 = ox + cols * r->cell_w + 2.0f;
        float track_x1 = track_x0 + track_w;
        float track_y0 = oy;
        float track_y1 = oy + grid_h;

        D2D1_COLOR_F track = to_d2d(r->bg_color);
        track.a = 0.35f;
        r->bg_brush->SetColor(track);
        D2D1_RECT_F tr = {track_x0, track_y0, track_x1, track_y1};
        r->render_target->FillRectangle(tr, r->bg_brush);

        float thumb_h = grid_h * ((float)sb->rows / (float)total_lines);
        if (thumb_h < r->cell_h) thumb_h = r->cell_h;
        if (thumb_h > grid_h) thumb_h = grid_h;

        float travel = grid_h - thumb_h;
        float denom = (float)(max_offset > 0 ? max_offset : 1);
        float live_to_top = denom > 0 ? ((float)sb->viewport_offset / denom) : 0.0f;
        float thumb_y = track_y1 - thumb_h - travel * live_to_top;
        if (thumb_y < track_y0) thumb_y = track_y0;
        if (thumb_y + thumb_h > track_y1) thumb_y = track_y1 - thumb_h;

        D2D1_COLOR_F thumb = to_d2d(r->fg_color);
        thumb.a = 0.45f;
        r->fg_brush->SetColor(thumb);
        D2D1_RECT_F th = {track_x0, thumb_y, track_x1, thumb_y + thumb_h};
        r->render_target->FillRectangle(th, r->fg_brush);
    }

    HRESULT hr=r->render_target->EndDraw();
    if (hr==D2DERR_RECREATE_TARGET) create_rt(r);
}

void renderer_set_font(Renderer *r, const wchar_t *family, float pt) {
    font_free(&r->font); font_init(&r->font,family,pt,r->dpi);
    r->cell_w=r->font.cell_width; r->cell_h=r->font.cell_height;
}
void renderer_update_dpi(Renderer *r, float dpi) {
    r->dpi=dpi; float pt=r->font.pt_size>0?r->font.pt_size:(r->cfg?r->cfg->font.size:13.0f);
    font_resize(&r->font,pt,dpi); r->cell_w=r->font.cell_width; r->cell_h=r->font.cell_height; create_rt(r);
}
void renderer_toggle_cursor_blink(Renderer *r) { r->cursor_blink_state=!r->cursor_blink_state; }
void renderer_destroy(Renderer *r) {
    if (r->blink_timer_id) { KillTimer(r->hwnd,r->blink_timer_id); r->blink_timer_id=0; }
    font_free(&r->font);
    if (r->cursor_brush) { r->cursor_brush->Release(); r->cursor_brush=NULL; }
    if (r->bg_brush)     { r->bg_brush->Release();     r->bg_brush=NULL; }
    if (r->fg_brush)     { r->fg_brush->Release();     r->fg_brush=NULL; }
    image_cache_destroy(r);
    if (r->render_target){ r->render_target->Release();r->render_target=NULL; }
    if (r->d2d_factory)  { r->d2d_factory->Release();  r->d2d_factory=NULL; }
}
