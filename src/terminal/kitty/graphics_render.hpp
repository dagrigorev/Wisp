/* Transliterated from Ghostty src/terminal/kitty/graphics_render.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * How a Kitty graphics image is positioned on screen, broken into the
 * fields a renderer needs rather than the ones the protocol carries.
 *
 * Wisp, differences in shape rather than behavior:
 *   - This file is ported in full. `kitty_graphics` was false when the
 *     surrounding subsystem was ported and this file was omitted; it is
 *     in scope now.
 *   - Upstream reaches Pin through `terminal = @import("../main.zig")`,
 *     which re-exports it. The port has no such aggregator header, so
 *     PageList is included directly.
 *   - Upstream's field defaults become member initializers. `top_left`
 *     has no default upstream and none here: there is no meaningful
 *     default pin, and a renderer that reads one before it is assigned
 *     would be drawing at an arbitrary place in the scrollback.
 *
 * Wisp: src/terminal/screen_view.h carries ScreenImagePlacement, which
 * re-derives this struct by hand for the Direct2D renderer, adding the
 * fields that renderer needs (an image id, a generation stamp, a cell
 * position rather than a pin, a z-index). That one is a renderer-facing
 * snapshot type and is not this; this is upstream's own placement, kept
 * so the subsystem is a complete port. They should be reconciled when
 * the renderer is wired to the ported render state rather than to
 * screen_view -- see docs/PORTING_CONVENTIONS.md on paraphrasing an
 * upstream type.
 */
#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_RENDER_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_RENDER_HPP

#include <stdint.h>
#include "../../vt/page_list.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_render {

typedef ::wisp::vt::PageList PageList;

/* A render placement is a way to position a Kitty graphics image onto
 * the screen. It is broken down into the fields that make it easier to
 * position the image using a renderer. */
struct Placement {
    /* The top-left corner of the image in grid coordinates. */
    PageList::Pin top_left;

    /* The offset in pixels from the top-left corner of the grid cell. */
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;

    /* The source rectangle of the image to render. This doesn't have to
     * match the size the destination size and the renderer is expected
     * to scale the image to fit the destination size. */
    uint32_t source_x = 0;
    uint32_t source_y = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;

    /* The final width/height of the image in pixels. */
    uint32_t dest_width = 0;
    uint32_t dest_height = 0;
};

} /* namespace graphics_render */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_RENDER_HPP */
