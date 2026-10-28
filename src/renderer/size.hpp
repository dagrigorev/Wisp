/* Transliterated from Ghostty src/renderer/size.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - PaddingBalance's `false`/`true` tags are `false_`/`true_`.
 *   - `Coordinate` (a tagged union) is a tag plus the three payloads.
 *   - `-|` (saturating subtraction) is `satSub`.
 *   - The unused scoped logger is omitted.
 */
#pragma once
#ifndef WISP_RENDERER_SIZE_HPP
#define WISP_RENDERER_SIZE_HPP

#include <assert.h>
#include <math.h>
#include <stdint.h>

#include "../vt/size.hpp"

namespace wisp {
namespace renderer {
namespace size {

inline uint32_t satSub(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }

/* Controls how extra whitespace around the terminal grid is distributed. */
enum class PaddingBalance {
    /* No balancing; padding is applied as specified explicitly. */
    false_,
    /* Balances padding but caps the top padding so the first row doesn't
     * drift too far from the top of the window. Excess vertical space is
     * shifted to the bottom. */
    true_,
    /* Distributes leftover space equally on all sides so the grid is
     * centered within the screen. */
    equal,
};

/* The dimensions of a single "cell" in the terminal grid.
 *
 * The dimensions are dependent on the current loaded set of font glyphs.
 * We calculate the width based on the widest character and the height based
 * on the height requirement for an underscore (the "lowest" -- visually --
 * character).
 *
 * The units for the width and height are in world space. They have to
 * be normalized for any renderer implementation. */
struct CellSize {
    uint32_t width;
    uint32_t height;
};

struct ScreenSize;
struct GridSize;

/* The padding to add to a screen. */
struct Padding {
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t right = 0;
    uint32_t left = 0;

    /* Returns padding that balances the whitespace around the screen
     * for the given grid and cell sizes. */
    static Padding balanced(const ScreenSize &screen, const GridSize &grid, const CellSize &cell);

    /* Add another padding to this one */
    Padding add(const Padding &other) const {
        Padding p;
        p.top = top + other.top;
        p.bottom = bottom + other.bottom;
        p.right = right + other.right;
        p.left = left + other.left;
        return p;
    }

    /* Equality test between two paddings. */
    bool eql(const Padding &other) const {
        return top == other.top && bottom == other.bottom && right == other.right && left == other.left;
    }
};

/* The dimensions of the screen that the grid is rendered to. This is the
 * terminal screen, so it is likely a subset of the window size. The dimensions
 * should be in pixels. */
struct ScreenSize {
    uint32_t width;
    uint32_t height;

    /* Subtract padding from the screen size. */
    ScreenSize subPadding(const Padding &padding) const {
        ScreenSize s;
        s.width = satSub(width, padding.left + padding.right);
        s.height = satSub(height, padding.top + padding.bottom);
        return s;
    }

    /* Calculates the amount of blank space around the grid. This is possible
     * when padding isn't balanced.
     *
     * The "self" screen size here should be the unpadded screen. */
    Padding blankPadding(const Padding &padding, const GridSize &grid, const CellSize &cell) const;

    /* Returns true if two sizes are equal. */
    bool equals(const ScreenSize &other) const { return width == other.width && height == other.height; }
};

/* The dimensions of the grid itself, in rows/columns units. */
struct GridSize {
    typedef ::wisp::vt::size::CellCountInt Unit;

    Unit columns = 0;
    Unit rows = 0;

    /* Initialize a grid size based on a screen and cell size. */
    static GridSize init(const ScreenSize &screen, const CellSize &cell) {
        GridSize result;
        result.update(screen, cell);
        return result;
    }

    /* Update the columns/rows for the grid based on the given screen and
     * cell size. */
    void update(const ScreenSize &screen, const CellSize &cell) {
        const float cell_width = (float)cell.width;
        const float cell_height = (float)cell.height;
        const float screen_width = (float)screen.width;
        const float screen_height = (float)screen.height;
        const Unit calc_cols = (Unit)(screen_width / cell_width);
        const Unit calc_rows = (Unit)(screen_height / cell_height);
        columns = calc_cols > 1 ? calc_cols : 1;
        rows = calc_rows > 1 ? calc_rows : 1;
    }

    /* Returns true if two sizes are equal. */
    bool equals(const GridSize &other) const { return columns == other.columns && rows == other.rows; }
};

inline Padding ScreenSize::blankPadding(const Padding &padding, const GridSize &grid, const CellSize &cell) const {
    const uint32_t grid_width = grid.columns * cell.width;
    const uint32_t grid_height = grid.rows * cell.height;
    const uint32_t padded_width = grid_width + (padding.left + padding.right);
    const uint32_t padded_height = grid_height + (padding.top + padding.bottom);

    /* Note these have to use a saturating subtraction to avoid underflow
     * because our padding can cause the padded sizes to be larger than
     * our real screen if the screen is shrunk to a minimal size such
     * as 1x1. */
    const uint32_t leftover_width = satSub(width, padded_width);
    const uint32_t leftover_height = satSub(height, padded_height);

    Padding p;
    p.top = 0;
    p.bottom = leftover_height;
    p.right = leftover_width;
    p.left = 0;
    return p;
}

inline Padding Padding::balanced(const ScreenSize &screen, const GridSize &grid, const CellSize &cell) {
    /* Turn our cell sizes into floats for the math */
    const float cell_width = (float)cell.width;
    const float cell_height = (float)cell.height;

    /* The size of our full grid */
    const float grid_width = (float)grid.columns * cell_width;
    const float grid_height = (float)grid.rows * cell_height;

    /* The empty space to the right of a line and bottom of the last row */
    const float space_right = (float)screen.width - grid_width;
    const float space_bot = (float)screen.height - grid_height;

    /* The padding is split equally along both axes. */
    const float padding_right = floorf(space_right / 2);
    const float padding_left = padding_right;

    const float padding_bot = floorf(space_bot / 2);
    const float padding_top = padding_bot;

    const float zero = 0;
    Padding p;
    p.top = (uint32_t)(padding_top > zero ? padding_top : zero);
    p.bottom = (uint32_t)(padding_bot > zero ? padding_bot : zero);
    p.right = (uint32_t)(padding_right > zero ? padding_right : zero);
    p.left = (uint32_t)(padding_left > zero ? padding_left : zero);
    return p;
}

/* All relevant sizes for a rendered terminal. These are all the sizes that
 * any functionality should need to know about the terminal in order to
 * convert between any coordinate systems.
 *
 * See the individual field type documentation for more information on each
 * field. One important note is that any pixel values should already be scaled
 * to the current DPI of the screen. If the DPI changes, the sizes should be
 * recalculated and we expect this to be done by the caller. */
struct Size {
    ScreenSize screen;
    CellSize cell;
    Padding padding;

    /* Return the grid size for this size. The grid size is calculated by
     * taking the screen size, removing padding, and dividing by the cell
     * dimensions. */
    GridSize grid() const { return GridSize::init(screen.subPadding(padding), cell); }

    /* The size of the terminal. This is the same as the screen without
     * padding. */
    ScreenSize terminal() const { return screen.subPadding(padding); }

    /* Set the padding to be balanced around the grid. The balanced
     * padding is calculated AFTER the explicit padding is taken
     * into account. */
    void balancePadding(const Padding &explicit_, PaddingBalance mode) {
        /* This ensure grid() does the right thing */
        padding = explicit_;

        /* Now we can calculate the balanced padding */
        padding = Padding::balanced(screen, grid(), cell);

        switch (mode) {
            case PaddingBalance::false_: assert(false && "unreachable"); break;
            case PaddingBalance::equal: break;
            case PaddingBalance::true_: {
                /* Cap the top padding to avoid excessive space above the
                 * first row. The maximum is the balanced explicit horizontal
                 * padding plus half a cell width. Any excess is shifted to
                 * the bottom. */
                const uint32_t max_top = (explicit_.left + explicit_.right + cell.width) / 2;
                const uint32_t vshift = satSub(padding.top, max_top);
                padding.top -= vshift;
                padding.bottom += vshift;
                break;
            }
        }
    }
};

/* A coordinate. This is defined as a tagged union to allow for different
 * coordinate systems to be represented.
 *
 * A coordinate is only valid within the context of a stable Size value.
 * If any of the sizes in the Size struct change, the coordinate is no
 * longer valid and must be recalculated. A conversion function is provided
 * to migrate to a new Size (which may result in failure).
 *
 * The coordinate systems are:
 *
 *   * surface: (0, 0) is the top-left of the surface (with padding). Negative
 *     values are allowed and are off the surface. Likewise, values greater
 *     than the surface size are off the surface. Units are pixels.
 *
 *   * terminal: (0, 0) is the top-left of the terminal grid. This is the
 *     same as the surface but with the padding removed. Negative values and
 *     values greater than the grid size are allowed and are off the terminal.
 *     Units are pixels.
 *
 *   * grid: (0, 0) is the top-left of the grid. Units are in cells. Negative
 *     values are not allowed but values greater than the grid size are
 *     possible and are off the grid.
 */
struct Coordinate {
    enum class Tag { surface, terminal, grid };
    struct Surface {
        double x;
        double y;
    };
    struct Terminal {
        double x;
        double y;
    };
    struct Grid {
        GridSize::Unit x;
        GridSize::Unit y;
    };

    Tag tag = Tag::surface;
    Surface surface = {0, 0};
    Terminal terminal = {0, 0};
    Grid grid = {0, 0};

    static Coordinate makeSurface(double x, double y) {
        Coordinate c;
        c.tag = Tag::surface;
        c.surface = {x, y};
        return c;
    }
    static Coordinate makeTerminal(double x, double y) {
        Coordinate c;
        c.tag = Tag::terminal;
        c.terminal = {x, y};
        return c;
    }
    static Coordinate makeGrid(GridSize::Unit x, GridSize::Unit y) {
        Coordinate c;
        c.tag = Tag::grid;
        c.grid = {x, y};
        return c;
    }

    /* Wisp: expectEqual on the union compares the tag and active payload. */
    bool operator==(const Coordinate &o) const {
        if (tag != o.tag) return false;
        switch (tag) {
            case Tag::surface: return surface.x == o.surface.x && surface.y == o.surface.y;
            case Tag::terminal: return terminal.x == o.terminal.x && terminal.y == o.terminal.y;
            case Tag::grid: return grid.x == o.grid.x && grid.y == o.grid.y;
        }
        return false;
    }

    /* Convert a coordinate to a different space within the same Size. */
    Coordinate convert(Tag to, const Size &size) const {
        /* Unlikely fast-path but avoid work. */
        if (tag == to) return *this;

        /* To avoid the combinatorial explosion of conversion functions, we
         * convert to the surface system first and then reconvert from there. */
        const Surface s = convertToSurface(size);

        switch (to) {
            case Tag::surface: return makeSurface(s.x, s.y);
            case Tag::terminal:
                return makeTerminal(s.x - (double)size.padding.left, s.y - (double)size.padding.top);
            case Tag::grid: {
                /* Get rid of the padding. */
                const Terminal term = makeSurface(s.x, s.y).convert(Tag::terminal, size).terminal;

                /* We need our grid to clamp */
                const GridSize g = size.grid();

                /* Calculate the grid position. */
                const double cell_width = (double)size.cell.width;
                const double cell_height = (double)size.cell.height;
                const double clamped_x = term.x > 0 ? term.x : 0;
                const double clamped_y = term.y > 0 ? term.y : 0;
                const GridSize::Unit col = (GridSize::Unit)(clamped_x / cell_width);
                const GridSize::Unit row = (GridSize::Unit)(clamped_y / cell_height);
                const GridSize::Unit max_col = (GridSize::Unit)(g.columns - 1);
                const GridSize::Unit max_row = (GridSize::Unit)(g.rows - 1);
                const GridSize::Unit clamped_col = col < max_col ? col : max_col;
                const GridSize::Unit clamped_row = row < max_row ? row : max_row;
                return makeGrid(clamped_col, clamped_row);
            }
        }
        return *this;
    }

    /* Convert a coordinate to the surface coordinate system. */
    Surface convertToSurface(const Size &size) const {
        switch (tag) {
            case Tag::surface: return surface;
            case Tag::terminal:
                return {terminal.x + (double)size.padding.left, terminal.y + (double)size.padding.top};
            case Tag::grid: {
                const double col = (double)grid.x;
                const double row = (double)grid.y;
                const double cell_width = (double)size.cell.width;
                const double cell_height = (double)size.cell.height;
                const double padding_left = (double)size.padding.left;
                const double padding_top = (double)size.padding.top;
                return {col * cell_width + padding_left, row * cell_height + padding_top};
            }
        }
        return surface;
    }
};

} /* namespace size */
} /* namespace renderer */
} /* namespace wisp */

#endif /* WISP_RENDERER_SIZE_HPP */
