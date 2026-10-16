#include "vt_pane.hpp"

#include <string.h>

#include <new>

#include "../vt/point.hpp"

namespace wisp {
namespace terminal {

namespace {

/* The pane that owns the handler, recovered from the handler's terminal.
 * The Handler's effects take a Handler*, and a Handler lives inside the
 * Stream inside the pane, so the pane is found through its terminal. */
VtPane *paneOf(stream_terminal::Handler *handler);

/* Every pane registers here so an effect can find its own pane. A terminal
 * belongs to exactly one pane, and a process has a handful of panes, so a
 * flat scan is cheaper than a map. */
struct Registry {
    static const size_t max_panes = 64;
    VtPane *panes[max_panes];
    size_t len;

    Registry() : len(0) {
        for (size_t i = 0; i < max_panes; i++) panes[i] = nullptr;
    }

    void add(VtPane *p) {
        for (size_t i = 0; i < max_panes; i++) {
            if (panes[i] == nullptr) {
                panes[i] = p;
                if (i + 1 > len) len = i + 1;
                return;
            }
        }
    }

    void remove(VtPane *p) {
        for (size_t i = 0; i < len; i++) {
            if (panes[i] == p) panes[i] = nullptr;
        }
    }

    VtPane *find(const vt::Terminal *t) const {
        for (size_t i = 0; i < len; i++) {
            if (panes[i] != nullptr && &panes[i]->terminal == t) return panes[i];
        }
        return nullptr;
    }
};

Registry &registry() {
    static Registry r;
    return r;
}

VtPane *paneOf(stream_terminal::Handler *handler) {
    if (handler == nullptr) return nullptr;
    return registry().find(handler->terminal);
}

void writePtyEffect(stream_terminal::Handler *handler, const char *data, size_t len) {
    VtPane *pane = paneOf(handler);
    if (pane == nullptr || pane->write_fn == nullptr) return;
    pane->write_fn(pane->ctx, data, len);
}

void titleChangedEffect(stream_terminal::Handler *handler) {
    VtPane *pane = paneOf(handler);
    if (pane == nullptr) return;
    /* The view carries the title, so refresh it before notifying. */
    const char *title = pane->terminal.getTitle();
    if (title == nullptr) {
        pane->view.title[0] = '\0';
    } else {
        const size_t cap = sizeof(pane->view.title) - 1;
        size_t n = strlen(title);
        if (n > cap) n = cap;
        memcpy(pane->view.title, title, n);
        pane->view.title[n] = '\0';
    }
    if (pane->title_fn != nullptr) pane->title_fn(pane->ctx);
}

void bellEffect(stream_terminal::Handler *handler) {
    VtPane *pane = paneOf(handler);
    if (pane == nullptr || pane->bell_fn == nullptr) return;
    pane->bell_fn(pane->ctx);
}

/* The absolute row index of a pin from the top of the page list. Used to
 * measure how far the viewport sits above the active area. */
size_t rowIndexOf(const vt::PageList *pages, const vt::PageList::Pin &pin) {
    size_t n = 0;
    for (const vt::PageList::Node *node = pages->pages.first; node != nullptr; node = node->next) {
        if (node == pin.node) return n + (size_t)pin.y;
        n += node->rows();
    }
    return n;
}

/* Translate one ported style into the renderer's flat attributes. A palette
 * color stays an index so the renderer's configured palette applies; an RGB
 * color is passed through with the 0xFF index sentinel. */
/* ── Images ──────────────────────────────────────────────────────────────
 *
 * Original to Wisp. Ghostty's renderer/image.zig does this job against
 * OpenGL/Metal; what is taken from it is the *shape* of the pass, not any
 * code: rebuild the placement list from scratch whenever storage is dirty,
 * skip placements whose bytes have not arrived or whose pin is garbage,
 * reject a placement wholly outside the viewport by comparing its screen
 * row span against the viewport's, keep the rest with a (possibly
 * negative) viewport-relative cell position so the drawing side can clip,
 * and sort the result by (z, image id) with the z==0 boundary splitting
 * the glyph pass. Everything below the viewport-row arithmetic is Wisp's,
 * because upstream gets its row numbers from `pointFromPin(.screen, ...)`
 * and Wisp needs a signed offset that survives a pin in scrollback. */

/* The viewport row a pin sits on, which may be negative (above the
 * viewport) or >= rows (below it). Both are legal for a placement that is
 * only partly visible, so this must not clamp, assert or wrap: it is
 * computed as a difference of absolute row indices in `ptrdiff_t`.
 * `pointFromPin(.viewport, ...)` cannot be used, because it returns null
 * for exactly the interesting case of a pin above the viewport. */
ptrdiff_t viewportRowOf(const vt::PageList *pages, const vt::PageList::Pin &pin,
                        const vt::PageList::Pin &viewport_top) {
    return (ptrdiff_t)rowIndexOf(pages, pin) - (ptrdiff_t)rowIndexOf(pages, viewport_top);
}

CellAttr attrOf(const vt::style::Style &style) {
    CellAttr a = style_table_default_attr();

    switch (style.fg_color.tag) {
    case vt::style::Style::Color::Tag::none: break;
    case vt::style::Style::Color::Tag::palette: a.fg_idx = style.fg_color.palette; break;
    case vt::style::Style::Color::Tag::rgb:
        a.fg_idx = 0xFF;
        a.fg_rgb = ((uint32_t)style.fg_color.rgb.r << 16) | ((uint32_t)style.fg_color.rgb.g << 8) |
                   (uint32_t)style.fg_color.rgb.b;
        break;
    }

    switch (style.bg_color.tag) {
    case vt::style::Style::Color::Tag::none: break;
    case vt::style::Style::Color::Tag::palette: a.bg_idx = style.bg_color.palette; break;
    case vt::style::Style::Color::Tag::rgb:
        a.bg_idx = 0xFF;
        a.bg_rgb = ((uint32_t)style.bg_color.rgb.r << 16) | ((uint32_t)style.bg_color.rgb.g << 8) |
                   (uint32_t)style.bg_color.rgb.b;
        break;
    }

    a.bold = style.flags.bold ? 1 : 0;
    a.italic = style.flags.italic ? 1 : 0;
    a.underline = style.flags.underline != sgr::Attribute::Underline::none ? 1 : 0;
    a.blink = style.flags.blink ? 1 : 0;
    a.reverse = style.flags.inverse ? 1 : 0;
    a.dim = style.flags.faint ? 1 : 0;
    a.strikethrough = style.flags.strikethrough ? 1 : 0;
    return a;
}

namespace kstore = ::wisp::terminal::kitty::graphics_storage;

typedef kstore::ImageStorage ImageStorage;

/* Publish one placement, already resolved to an anchoring pin plus cell
 * offsets (relative placements accumulate offsets along their parent
 * chain, exactly as upstream's `Origin` does). Returns without publishing
 * anything when the placement has no drawable pixels this frame. */
void publishPlacement(VtPane *pane, const vt::Terminal *t, const vt::PageList *pages,
                      const vt::PageList::Pin &viewport_top,
                      const kstore::Image &image, const ImageStorage::Placement &p,
                      const vt::PageList::Pin *pin, int32_t horizontal_offset,
                      int32_t vertical_offset) {
    /* The bytes have not arrived yet. The placement stays in storage; it
     * just has nothing to draw. */
    if (image.data.isPending()) return;

    /* A pin whose tracked content was pruned from history has no
     * position. Checked before the pin is dereferenced for a row index,
     * which is what keeps a scrollback pin from being walked at all. */
    if (pin == nullptr || pin->garbage) return;

    const uint8_t *pixels = nullptr;
    size_t pixels_len = 0;
    if (!image.renderData().bytes(&pixels, &pixels_len)) return;
    /* Only RGBA8888 is published; sync() converts the stored image first. */
    if (image.format != kitty::graphics::Transmission::Format::rgba) return;
    if ((size_t)image.width * (size_t)image.height * 4 > pixels_len) return;

    const ImageStorage::Placement::GridSize grid = p.gridSize(image, t);
    if (grid.cols == 0 || grid.rows == 0) return;

    const ImageStorage::Placement::PixelSize dest = p.pixelSize(image, t);
    if (dest.width == 0 || dest.height == 0) return;

    /* Signed and widened: a relative placement's chain offsets can hang it
     * above or to the left of its root. */
    const ptrdiff_t top = viewportRowOf(pages, *pin, viewport_top) + (ptrdiff_t)vertical_offset;
    const ptrdiff_t bottom = top + (ptrdiff_t)grid.rows - 1;
    const ptrdiff_t left = (ptrdiff_t)pin->x + (ptrdiff_t)horizontal_offset;
    const ptrdiff_t right = left + (ptrdiff_t)grid.cols - 1;

    /* Entirely outside the viewport: not published at all. Anything that
     * overlaps it even by a row is published with its real (possibly
     * negative) position and clipped by the renderer. */
    if (top >= (ptrdiff_t)pane->view.rows || bottom < 0) return;
    if (left >= (ptrdiff_t)pane->view.cols || right < 0) return;

    /* A position that does not fit an int32 has no renderable pixels; this
     * is upstream's `std.math.cast(i32, ...) orelse return`. */
    if (top > 2147483647 || top < (-2147483647 - 1)) return;
    if (left > 2147483647 || left < (-2147483647 - 1)) return;

    const ImageStorage::Placement::CellOffset cell_offset = p.cellOffset(t);
    const ImageStorage::Placement::SourceRect source = p.sourceRect(image);
    if (source.width == 0 || source.height == 0) return;

    ScreenImagePlacement out;
    memset(&out, 0, sizeof(out));
    out.image_id = image.id;
    out.generation = image.generation;
    out.pixels = pixels;
    out.pixel_width = image.width;
    out.pixel_height = image.height;
    out.dest_col = (int32_t)left;
    out.dest_row = (int32_t)top;
    out.cell_offset_x = cell_offset.x;
    out.cell_offset_y = cell_offset.y;
    out.dest_width_px = dest.width;
    out.dest_height_px = dest.height;
    out.source_x = source.x;
    out.source_y = source.y;
    out.source_width = source.width;
    out.source_height = source.height;
    out.z = p.z;
    (void)screen_view_images_push(&pane->view, &out);
}

void syncImages(VtPane *pane) {
    vt::Screen *screen = pane->terminal.screens.active;
    ImageStorage *storage = &screen->kitty_images;
    vt::PageList *pages = &screen->pages;

    screen_view_images_begin(&pane->view);

    /* Publish the live image IDs before anything else, so the renderer can
     * release the bitmaps of images that left storage even in a frame where
     * no placement is visible. */
    {
        ImageStorage::ImageMap::Iterator it = storage->images.iterator();
        ImageStorage::ImageMap::Entry entry;
        while (it.next(&entry)) {
            (void)screen_view_images_push_id(&pane->view, entry.value_ptr->id);
        }
    }

    /* Everything published is RGBA8888, so the stored representation is
     * normalized here rather than in the renderer: the conversion is a
     * no-op for an image that is already RGBA, and for one that is not it
     * happens once (it rewrites the stored bytes and stamps a fresh
     * generation), not once per frame. Decoding has already happened in
     * storage; this is a swizzle, never a decode. */
    {
        ImageStorage::ImageMap::Iterator it = storage->images.iterator();
        ImageStorage::ImageMap::Entry entry;
        while (it.next(&entry)) {
            (void)storage->convertImageToRgba(zigstd::c_allocator(), entry.value_ptr);
        }
    }

    const vt::PageList::Pin viewport_top = pages->getTopLeft(vt::point::Tag::viewport);

    ImageStorage::PlacementMap::Iterator it = storage->placements.iterator();
    ImageStorage::PlacementMap::Entry entry;
    while (it.next(&entry)) {
        const ImageStorage::PlacementKey key = *entry.key_ptr;
        const ImageStorage::Placement &p = *entry.value_ptr;

        const vt::Maybe<kstore::Image> image = storage->imageById(key.image_id);
        if (!image.has) continue;

        switch (p.location.tag) {
        case ImageStorage::Placement::Location::Tag::pin:
            publishPlacement(pane, &pane->terminal, pages, viewport_top, image.value, p,
                             p.location.pin, 0, 0);
            break;

        case ImageStorage::Placement::Location::Tag::relative: {
            /* An unresolvable chain is never drawn; upstream skips it too. */
            const vt::Maybe<ImageStorage::ResolvedChain> chain =
                storage->resolveChain(p.location.relative);
            if (!chain.has) break;
            if (chain.value.root.location.tag != ImageStorage::Placement::Location::Tag::pin)
                break;
            publishPlacement(pane, &pane->terminal, pages, viewport_top, image.value, p,
                             chain.value.root.location.pin, chain.value.horizontal_offset,
                             chain.value.vertical_offset);
            break;
        }

        case ImageStorage::Placement::Location::Tag::virtual_:
            /* Virtual placements are positioned by unicode placeholder
             * cells, not by the placement itself. Wisp does not scan for
             * placeholders yet (upstream's `prepKittyVirtualPlacement`
             * and the `kitty_virtual` every-frame rebuild), so these
             * publish nothing. A relative placement rooted at a virtual
             * one is skipped above for the same reason. */
            break;
        }
    }

    screen_view_images_end(&pane->view);

    /* The snapshot now reflects storage, so the terminal's "something
     * changed" flag has been consumed. Upstream clears it in `kittyUpdate`
     * with the same `defer storage.dirty = false`. */
    storage->dirty = false;
}

} /* namespace */

bool VtPane::init(unsigned cols, unsigned rows, size_t scrollback_bytes) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    if (!screen_view_init(&view, (int)cols, (int)rows)) return false;

    vt::Terminal::Options opts((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
    opts.max_scrollback_bytes = vt::Maybe<size_t>(scrollback_bytes);
    if (!vt::Terminal::init(zigstd::c_allocator(), opts, &terminal)) {
        screen_view_free(&view);
        return false;
    }

    Handler handler = Handler::init(&terminal);
    handler.effects.write_pty = &writePtyEffect;
    handler.effects.title_changed = &titleChangedEffect;
    handler.effects.bell = &bellEffect;

    Stream::Options stream_opts;
    stream_opts.allocator = true;
    stream = new (std::nothrow) Stream(handler, stream_opts);
    if (stream == nullptr) {
        terminal.deinit(zigstd::c_allocator());
        screen_view_free(&view);
        return false;
    }

    registry().add(this);
    sync();
    return true;
}

void VtPane::deinit() {
    registry().remove(this);
    if (stream != nullptr) {
        stream->deinit();
        delete stream;
        stream = nullptr;
    }
    terminal.deinit(zigstd::c_allocator());
    screen_view_free(&view);
}

void VtPane::feed(const char *data, size_t len) {
    if (stream == nullptr || len == 0) return;
    stream->nextSlice(data, len);
    sync();
}

bool VtPane::resize(unsigned cols, unsigned rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (stream == nullptr) return false;

    vt::Terminal::Resize r((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
    /* Carry the pixel geometry through every resize; a resize that dropped
     * it would leave the Kitty geometry layer dividing by a zero cell size
     * until the next setCellSizePx. */
    if (cell_px_w > 0 && cell_px_h > 0) {
        vt::Terminal::Resize::CellSize cs;
        cs.width = cell_px_w;
        cs.height = cell_px_h;
        r.cell_size_px = vt::Maybe<vt::Terminal::Resize::CellSize>(cs);
    }
    if (!stream->handler.resize(r)) return false;
    if (!screen_view_resize(&view, (int)cols, (int)rows)) return false;
    sync();
    return true;
}

void VtPane::scrollViewport(int delta_rows) {
    /* Positive scrolls back through scrollback, which is negative in the
     * page list's row direction. */
    terminal.screens.active->pages.scroll(
        vt::PageList::Scroll::deltaRow(-(ptrdiff_t)delta_rows));
    sync();
}

void VtPane::pageViewport(int pages) {
    scrollViewport(pages * view.rows);
}

void VtPane::resetViewport() {
    terminal.screens.active->pages.scroll(vt::PageList::Scroll::active());
    sync();
}

void VtPane::sync() {
    vt::Screen *screen = terminal.screens.active;
    vt::PageList *pages = &screen->pages;

    const int rows = view.rows;
    const int cols = view.cols;

    for (int y = 0; y < rows; y++) {
        ScreenCell *out = view.cells + (size_t)y * (size_t)cols;

        const vt::Maybe<vt::PageList::Pin> pin =
            pages->pin(vt::point::Point::viewport(0, (uint32_t)y));
        if (!pin.has) {
            /* Below the last row the page list has; blank it. */
            const ScreenCell blank = screen_cell_blank();
            for (int x = 0; x < cols; x++) out[x] = blank;
            continue;
        }

        const vt::page::Page *page = pin.value.node->page();
        const vt::page::Row *row = pin.value.rowAndCell().row;
        const vt::page::Cell *cells = page->getCells(row);
        const int page_cols = (int)page->size.cols;

        for (int x = 0; x < cols; x++) {
            ScreenCell c = screen_cell_blank();
            if (x < page_cols) {
                const vt::page::Cell *cell = &cells[x];
                const uint32_t cp = cell->codepoint();
                c.ch = cp != 0 ? cp : (uint32_t)' ';
                c.wide = cell->wide() == vt::page::Cell::Wide::wide ? 1 : 0;
                c.wide_cont = cell->wide() == vt::page::Cell::Wide::spacer_tail ? 1 : 0;

                if (cell->hasStyling()) {
                    const vt::style::Style *style =
                        page->styles.get((const void *)page->memory, cell->style_id());
                    if (style != nullptr) {
                        bool ok = false;
                        const uint16_t id = style_table_intern(view.styles, attrOf(*style), &ok);
                        if (ok) c.style_id = id;
                    }
                }
            }
            out[x] = c;
        }
    }

    /* Cursor: the terminal tracks it in the active area, which is only
     * visible when the viewport is at the bottom. */
    const size_t total_rows = pages->totalRows();
    const size_t active_rows = (size_t)rows;
    view.scrollback_count = total_rows > active_rows ? (int)(total_rows - active_rows) : 0;

    {
        const vt::PageList::Pin active_top = pages->getTopLeft(vt::point::Tag::active);
        const vt::PageList::Pin viewport_top = pages->getTopLeft(vt::point::Tag::viewport);
        const size_t active_idx = rowIndexOf(pages, active_top);
        const size_t viewport_idx = rowIndexOf(pages, viewport_top);
        view.viewport_offset = active_idx > viewport_idx ? (int)(active_idx - viewport_idx) : 0;
    }

    view.cursor_x = (int)screen->cursor.x;
    view.cursor_y = (int)screen->cursor.y + view.viewport_offset;
    view.cursor_visible = terminal.modes.get(modes::Mode::cursor_visible) &&
                          view.cursor_y >= 0 && view.cursor_y < rows;

    view.alt_screen_active = terminal.screens.active_key == vt::ScreenSet::Key::alternate;
    view.bracketed_paste = terminal.modes.get(modes::Mode::bracketed_paste);
    view.app_cursor_keys = terminal.modes.get(modes::Mode::cursor_keys);

    {
        const char *title = terminal.getTitle();
        if (title == nullptr) {
            view.title[0] = '\0';
        } else {
            const size_t cap = sizeof(view.title) - 1;
            size_t n = strlen(title);
            if (n > cap) n = cap;
            memcpy(view.title, title, n);
            view.title[n] = '\0';
        }
    }

    /* Last, because it reads the viewport position this function just
     * settled on. */
    syncImages(this);
}

void VtPane::setCellSizePx(unsigned cell_w, unsigned cell_h) {
    if (cell_w == cell_px_w && cell_h == cell_px_h) return;
    cell_px_w = cell_w;
    cell_px_h = cell_h;
    if (stream == nullptr) return;

    /* The whole Kitty geometry layer (pixelSize, gridSize, cellOffset)
     * divides terminal.width_px by terminal.cols, so without this the cell
     * size is zero and every placement resolves to an empty rect. The
     * terminal only learns its pixel geometry through a resize, so this
     * re-issues the current grid size carrying the cell size. */
    vt::Terminal::Resize r((vt::size::CellCountInt)view.cols, (vt::size::CellCountInt)view.rows);
    if (cell_w > 0 && cell_h > 0) {
        vt::Terminal::Resize::CellSize cs;
        cs.width = cell_w;
        cs.height = cell_h;
        r.cell_size_px = vt::Maybe<vt::Terminal::Resize::CellSize>(cs);
    }
    (void)stream->handler.resize(r);
    sync();
}

} /* namespace terminal */
} /* namespace wisp */
