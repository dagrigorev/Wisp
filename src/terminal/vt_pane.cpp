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
}

} /* namespace terminal */
} /* namespace wisp */
