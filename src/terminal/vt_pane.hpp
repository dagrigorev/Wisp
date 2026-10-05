#pragma once
#ifndef WISP_TERMINAL_VT_PANE_HPP
#define WISP_TERMINAL_VT_PANE_HPP

/* One terminal pane: the ported terminal (src/vt) plus the renderer's view.
 *
 * This is the boundary between the port and the Win32 app. The app feeds pty
 * bytes in, reads the ScreenBuffer snapshot out, and sends the terminal's
 * replies (DA, DSR, XTGETTCAP, clipboard, ...) back to the pty through the
 * write_pty callback.
 *
 * It replaced vt_parser.c, which interpreted escape sequences into the old
 * screen.c state machine. Sequence handling now lives entirely in
 * terminal/stream_terminal.hpp on top of vt::Terminal. */

#include <stddef.h>
#include <stdint.h>

/* windows.h defines RGB as a macro and the port has a type by that name, so
 * the macro is dropped before the port's headers are read. Nothing in Wisp
 * uses the macro. */
#ifdef RGB
#undef RGB
#endif

#include "stream_terminal.hpp"

#include "screen_view.h"

#ifdef RGB
#undef RGB
#endif

namespace wisp {
namespace terminal {

struct VtPane {
    typedef ::wisp::terminal::stream_terminal::Handler Handler;
    typedef ::wisp::terminal::stream::Stream<Handler> Stream;

    /* Called with the bytes the terminal wants written back to the pty.
     * The data is borrowed for the duration of the call. */
    typedef void (*WriteFn)(void *ctx, const char *data, size_t len);

    /* Called when the title changed; read it from `view.title`. */
    typedef void (*TitleFn)(void *ctx);

    /* Called when the program rang the bell. */
    typedef void (*BellFn)(void *ctx);

    /* Initialize a pane of the given size. `scrollback_bytes` is the
     * scrollback limit; zero disables scrollback entirely. Returns false on
     * allocation failure, in which case nothing was allocated. */
    bool init(unsigned cols, unsigned rows, size_t scrollback_bytes);
    void deinit();

    /* Feed pty output. Replies go to `write_fn`; the view is refreshed. */
    void feed(const char *data, size_t len);

    /* Resize the terminal and the view. Returns false on allocation
     * failure, in which case the pane keeps its old size. */
    bool resize(unsigned cols, unsigned rows);

    /* Viewport movement. Positive scrolls back (up) through scrollback. */
    void scrollViewport(int delta_rows);
    void pageViewport(int pages);
    void resetViewport();

    /* Rebuild `view` from the terminal state. Called after every feed; call
     * it directly after moving the viewport or changing the terminal by
     * other means. */
    void sync();

    /* The renderer's snapshot. Valid until the next feed, resize or sync. */
    ScreenBuffer view;

    /* Embedder callbacks and their context. Set after init. */
    void *ctx;
    WriteFn write_fn;
    TitleFn title_fn;
    BellFn bell_fn;

    /* The ported terminal and the sequence stream driving it. Public so the
     * app can reach terminal state the view does not carry (selection,
     * hyperlinks, the formatters). */
    vt::Terminal terminal;
    Stream *stream;

    VtPane() : view(), ctx(nullptr), write_fn(nullptr), title_fn(nullptr), bell_fn(nullptr),
               terminal(), stream(nullptr) {}
};

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_VT_PANE_HPP */
