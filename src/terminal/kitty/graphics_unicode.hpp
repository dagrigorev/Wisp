/* Transliterated from Ghostty src/terminal/kitty/graphics_unicode.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * This file contains various logic and data for working with the
 * Kitty graphics protocol unicode placeholder, virtual placement feature.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: this file is ported IN FULL, including all 16 upstream tests
 * (tests/test_kitty_graphics_unicode_port.cpp). Upstream gates nothing in
 * this file on build_options; the port's `kitty_graphics = false` note in
 * docs/PORTING_CONVENTIONS.md no longer applies here, Kitty graphics being
 * in scope now. Nothing is gated on glyph_protocol or tmux_control_mode.
 *
 * Wisp: departures from a mechanical mapping:
 *
 *   - `../../quirks.zig` is not ported in Wisp. The only thing this file
 *     uses from it is `inlineAssert`, which is `std.debug.assert` with an
 *     `inline` call modifier; it maps to plain `assert` from <assert.h>,
 *     as `src/vt/search/viewport.hpp` already does.
 *
 *   - `?T` is `Maybe<T>` (from src/vt/page_list.hpp), and an iterator's
 *     `?T next()` is `bool next(T *out)`, matching the port's
 *     PageList.RowIterator.
 *
 *   - `Error!RenderPlacement` is an `Error` return (with `Error::none` for
 *     success) plus an out-parameter.
 *
 *   - `log.warn` is a comment; there is no logging in the port.
 *
 *   - `Placement.renderPlacement` and `Placement.grid` take the image
 *     storage and the image as TEMPLATE parameters rather than as
 *     `*const ImageStorage` / `*const Image`. Upstream's `ImageStorage`
 *     (graphics_storage.zig, 4664 lines) and `Image` (graphics_image.zig,
 *     2161 lines) are not ported yet and are being ported separately; this
 *     file only ever touches three things on them, so templating lets the
 *     real ports drop in unchanged instead of this file re-porting, and
 *     then colliding with, a slice of a sibling file. The required shape
 *     is exactly:
 *
 *       img->width, img->height                       (uint32_t)
 *       storage->placeholderTarget(image_id, place_id) -> a Maybe-like
 *           value with `.has` and `.value.placement.rows` /
 *           `.value.placement.columns` (uint32_t)
 *
 *     If the ported ImageStorage spells `placeholderTarget` with a
 *     bool + out-param instead of `Maybe`, only that one call needs
 *     adapting.
 *
 *   - `RenderPlacement` is `terminal.kitty.graphics.RenderPlacement`, i.e.
 *     the `Placement` struct of graphics_render.zig (27 lines), which
 *     graphics_render.hpp supplies as `graphics::render::Placement`.
 *     Until that sibling existed this file transliterated the struct
 *     inline, behind a `__has_include` check that preferred the sibling
 *     if it ever appeared. It has appeared, so the copy and the check are
 *     gone.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_UNICODE_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_UNICODE_HPP

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "../../vt/page.hpp"
#include "../../vt/page_list.hpp"
#include "../../vt/size.hpp"
#include "../../vt/style.hpp"

/* Wisp: graphics_render.hpp supplies graphics::render::Placement, which
 * RenderPlacement below is an alias for. This was once a
 * __has_include check with the struct transliterated inline as a
 * fallback, because this file was ported before that one existed. The
 * sibling exists now, so the include is plain. */
#include "graphics_render.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics {


namespace graphics_unicode {

typedef ::wisp::vt::PageList PageList;
typedef ::wisp::vt::PageList::Pin Pin;
typedef ::wisp::vt::page::Cell Cell;
typedef ::wisp::vt::style::Style Style;
typedef ::wisp::vt::size::CellCountInt CellCountInt;
typedef render::Placement RenderPlacement;

/* Wisp: const log = std.log.scoped(.kitty_gfx); — no logging in the port. */

/// Codepoint for the unicode placeholder character.
static const uint32_t placeholder = 0x10EEEE;

struct IncompletePlacement;

/// A virtual placement in the terminal. This can represent more than
/// one cell if the cells combine to be a run.
struct Placement {
    /// The top-left pin of the placement. This can be used to get the
    /// screen x/y.
    Pin pin;

    /// The image ID and placement ID for this virtual placement. The
    /// image ID is encoded in the fg color (plus optional a 8-bit high
    /// value in the 3rd diacritic). The placement ID is encoded in the
    /// underline color (optionally).
    uint32_t image_id;
    uint32_t placement_id;

    /// Starting row/col index for the image itself. This is the "fragment"
    /// of the image we want to show in this placement. This is 0-indexed.
    uint32_t col;
    uint32_t row;

    /// The width/height in cells of this placement.
    uint32_t width;
    uint32_t height;

    Placement()
        : pin(), image_id(0), placement_id(0), col(0), row(0), width(0), height(0) {}

    /* Wisp: pub const Error = error{...}; `none` is success. */
    enum class Error {
        none,
        PlacementGridOutOfBounds,
        PlacementMissingPlacement,
    };

    /// Take this virtual placement and convert it to a render placement.
    /* Wisp: Error!RenderPlacement -> Error plus an out-parameter. See the
     * header note for the storage/image template shape. */
    template <typename ImageStorageT, typename ImageT>
    Error renderPlacement(
        const ImageStorageT *storage,
        const ImageT *img,
        uint32_t cell_width,
        uint32_t cell_height,
        RenderPlacement *out) const {
        // In this function, there is a variable naming convention to try
        // to make it slightly less confusing. The prefix will tell you what
        // coordinate/size space a variable lives in:
        // - img_* is for the original image
        // - p_* is for the final placement
        // - vp_* is for the virtual placement

        // Determine the grid size that this virtual placement fits into.
        Grid p_grid;
        {
            const Error err = grid(storage, img, cell_width, cell_height, &p_grid);
            if (err != Error::none) return err;
        }

        // From here on out we switch to floating point math. These are
        // constants that we'll reference repeatedly.
        const double img_width_f64 = (double)img->width;
        const double img_height_f64 = (double)img->height;

        // Next we have to fit the source image into the grid size while preserving
        // aspect ratio. We will center the image horizontally/vertically if
        // necessary.
        struct PScale {
            /// The offsets are pixels from the top-left of the placement-sized
            /// image in order to center the image as necessary.
            double x_offset;
            double y_offset;

            /// The multipliers to apply to the width/height of the original
            /// image size in order to reach the placement size.
            double x_scale;
            double y_scale;

            /* Wisp: upstream's field defaults are all 0, and each branch
             * below sets only three of the four, so the fourth must be
             * zero-initialized here too. */
            PScale() : x_offset(0), y_offset(0), x_scale(0), y_scale(0) {}
        };
        PScale p_scale;
        {
            const double p_rows_px = (double)(p_grid.rows * cell_height);
            const double p_cols_px = (double)(p_grid.columns * cell_width);
            if (img_width_f64 * p_rows_px > img_height_f64 * p_cols_px) {
                // Image is wider than the grid, fit width and center height
                const double x_scale = p_cols_px / (img_width_f64 > 1 ? img_width_f64 : 1);
                const double y_scale = x_scale;
                const double y_offset = (p_rows_px - img_height_f64 * y_scale) / 2;
                p_scale.x_scale = x_scale;
                p_scale.y_scale = y_scale;
                p_scale.y_offset = y_offset;
            } else {
                // Image is taller than the grid, fit height and center width
                const double y_scale = p_rows_px / (img_height_f64 > 1 ? img_height_f64 : 1);
                const double x_scale = y_scale;
                const double x_offset = (p_cols_px - img_width_f64 * x_scale) / 2;
                p_scale.x_scale = x_scale;
                p_scale.y_scale = y_scale;
                p_scale.x_offset = x_offset;
            }
        }

        // Scale our original image according to the aspect ratio
        // and padding calculated for p_scale.
        struct ImgScaled {
            double x_offset;
            double y_offset;
            double width;
            double height;
            ImgScaled() : x_offset(0), y_offset(0), width(0), height(0) {}
        };
        ImgScaled img_scaled;
        {
            const double x_offset = p_scale.x_offset / p_scale.x_scale;
            const double y_offset = p_scale.y_offset / p_scale.y_scale;
            const double width = img_width_f64 + (x_offset * 2);
            const double height = img_height_f64 + (y_offset * 2);
            img_scaled.x_offset = x_offset;
            img_scaled.y_offset = y_offset;
            img_scaled.width = width;
            img_scaled.height = height;
        }

        // Calculate the source rectangle for the scaled image. These
        // coordinates are in the scaled image space.
        struct ImgScaleSource {
            double x;
            double y;
            double width;
            double height;
            ImgScaleSource() : x(0), y(0), width(0), height(0) {}
        };
        ImgScaleSource img_scale_source;
        {
            // Float-converted values we already have
            const double vp_width = (double)this->width;
            const double vp_height = (double)this->height;
            const double vp_col = (double)this->col;
            const double vp_row = (double)this->row;
            const double p_grid_cols = (double)p_grid.columns;
            const double p_grid_rows = (double)p_grid.rows;

            // Calculate the scaled source rectangle for the image, undoing
            // the aspect ratio scaling as necessary.
            const double width = img_scaled.width * (vp_width / p_grid_cols);
            const double height = img_scaled.height * (vp_height / p_grid_rows);
            const double x = img_scaled.width * (vp_col / p_grid_cols);
            const double y = img_scaled.height * (vp_row / p_grid_rows);

            img_scale_source.width = width;
            img_scale_source.height = height;
            img_scale_source.x = x;
            img_scale_source.y = y;
        }

        // The destination rectangle. The x/y is specified by offsets from
        // the top-left since that's how our RenderPlacement works.
        struct PDest {
            double x_offset;
            double y_offset;
            double width;
            double height;
            PDest() : x_offset(0), y_offset(0), width(0), height(0) {}
        };
        PDest p_dest;
        {
            double x_offset = 0;
            double y_offset = 0;
            double width = (double)(this->width * cell_width);
            double height = (double)(this->height * cell_height);

            if (img_scale_source.y < img_scaled.y_offset) {
                // If our source rect y is within the offset area, we need to
                // adjust our source rect and destination since the source texture
                // doesn't actually have the offset area blank.
                const double offset = img_scaled.y_offset - img_scale_source.y;
                img_scale_source.height -= offset;
                y_offset = offset;
                height -= offset * p_scale.y_scale;
                img_scale_source.y = 0;

                // If our height is greater than our original height,
                // bring it back down. This addresses the case where the top
                // and bottom offsets are both used.
                if (img_scale_source.height > img_height_f64) {
                    img_scale_source.height = img_height_f64;
                    height = img_height_f64 * p_scale.y_scale;
                }
            } else if (img_scale_source.y + img_scale_source.height >
                       img_scaled.height - img_scaled.y_offset) {
                // if our y is in our bottom offset area, we need to shorten the
                // source to fit in the cell.
                img_scale_source.y -= img_scaled.y_offset;
                img_scale_source.height = img_scaled.height - img_scaled.y_offset - img_scale_source.y;
                img_scale_source.height -= img_scaled.y_offset;
                height = img_scale_source.height * p_scale.y_scale;
            } else {
                img_scale_source.y -= img_scaled.y_offset;
            }

            if (img_scale_source.x < img_scaled.x_offset) {
                // If our source rect x is within the offset area, we need to
                // adjust our source rect and destination since the source texture
                // doesn't actually have the offset area blank.
                const double offset = img_scaled.x_offset - img_scale_source.x;
                img_scale_source.width -= offset;
                x_offset = offset;
                width -= offset * p_scale.x_scale;
                img_scale_source.x = 0;

                // If our width is greater than our original width,
                // bring it back down. This addresses the case where the left
                // and right offsets are both used.
                if (img_scale_source.width > img_width_f64) {
                    img_scale_source.width = img_width_f64;
                    width = img_width_f64 * p_scale.x_scale;
                }
            } else if (img_scale_source.x + img_scale_source.width >
                       img_scaled.width - img_scaled.x_offset) {
                // if our x is in our right offset area, we need to shorten the
                // source to fit in the cell.
                img_scale_source.x -= img_scaled.x_offset;
                img_scale_source.width = img_scaled.width - img_scaled.x_offset - img_scale_source.x;
                img_scale_source.width -= img_scaled.x_offset;
                width = img_scale_source.width * p_scale.x_scale;
            } else {
                img_scale_source.x -= img_scaled.x_offset;
            }

            // If our modified source width/height is less than zero then
            // we render nothing because it means we're rendering outside
            // of the visible image.
            if (img_scale_source.width <= 0 || img_scale_source.height <= 0) {
                RenderPlacement rp;
                rp.top_left = this->pin;
                *out = rp;
                return Error::none;
            }

            p_dest.x_offset = x_offset * p_scale.x_scale;
            p_dest.y_offset = y_offset * p_scale.y_scale;
            p_dest.width = width;
            p_dest.height = height;
        }
        /* Wisp: upstream's commented-out log.warn of the intermediates is
         * dropped; there is no logging in the port. */

        RenderPlacement rp;
        rp.top_left = this->pin;
        rp.offset_x = (uint32_t)::round(p_dest.x_offset);
        rp.offset_y = (uint32_t)::round(p_dest.y_offset);
        rp.source_x = (uint32_t)::round(img_scale_source.x);
        rp.source_y = (uint32_t)::round(img_scale_source.y);
        rp.source_width = (uint32_t)::round(img_scale_source.width);
        rp.source_height = (uint32_t)::round(img_scale_source.height);
        rp.dest_width = (uint32_t)::round(p_dest.width);
        rp.dest_height = (uint32_t)::round(p_dest.height);
        *out = rp;
        return Error::none;
    }

    /* Wisp: the anonymous return struct of grid(). */
    struct Grid {
        uint32_t rows;
        uint32_t columns;
        Grid() : rows(0), columns(0) {}
    };

    // Calculate the grid size for the placement. For virtual placements,
    // we use the requested row/cols. If either isn't specified, we choose
    // the best size based on the image size to fit the entire image in its
    // original size.
    //
    // This part of the code does NOT do preserve any aspect ratios. Its
    // dumbly fitting the image into the grid size -- possibly user specified.
    template <typename ImageStorageT, typename ImageT>
    Error grid(
        const ImageStorageT *storage,
        const ImageT *image,
        uint32_t cell_width,
        uint32_t cell_height,
        Grid *out) const {
        // Get the placement. If an ID is specified we look for the exact one.
        // If no ID, then we find the first virtual placement for this image.
        /* Wisp: `auto` so this file does not have to name the sibling
         * ImageStorage's target/placement types. */
        const auto target_ = storage->placeholderTarget(this->image_id, this->placement_id);
        if (!target_.has) return Error::PlacementMissingPlacement;
        const auto placement = target_.value.placement;

        // Use requested rows/columns if specified
        // For unspecified rows/columns, calculate based on the image size.
        uint32_t rows = (uint32_t)placement.rows;
        uint32_t columns = (uint32_t)placement.columns;
        if (rows == 0) rows = (image->height + cell_height - 1) / cell_height;
        if (columns == 0) columns = (image->width + cell_width - 1) / cell_width;
        /* Wisp: std.math.cast(terminal.size.CellCountInt, x) orelse error —
         * CellCountInt is u16, so the cast fails above 0xFFFF. The struct
         * fields themselves stay u32, as upstream. */
        const uint32_t cell_count_int_max = ((uint32_t)1 << (sizeof(CellCountInt) * 8)) - 1;
        if (rows > cell_count_int_max) return Error::PlacementGridOutOfBounds;
        if (columns > cell_count_int_max) return Error::PlacementGridOutOfBounds;
        out->rows = rows;
        out->columns = columns;
        return Error::none;
    }
};

/// Get the row/col index for a diacritic codepoint. These are 0-indexed.
/* Wisp: forward declaration; the table and getIndex are defined after
 * IncompletePlacement, as upstream orders them, and IncompletePlacement.init
 * calls getIndex. */
inline ::wisp::vt::Maybe<uint32_t> getIndex(uint32_t cp);

/// IncompletePlacement is the placement information present in a single
/// cell. It is "incomplete" because the specification allows for missing
/// diacritics and so on that continue from previous valid placements.
struct IncompletePlacement {
    /// The pin of the cell that created this incomplete placement.
    Pin pin;

    /// Lower 24 bits of the image ID. This is specified in the fg color
    /// and is always required.
    /* Wisp: u24 -> uint32_t holding at most 24 bits. */
    uint32_t image_id_low;

    /// Higher 8 bits of the image ID specified using the 3rd diacritic.
    /// This is optional.
    ::wisp::vt::Maybe<uint8_t> image_id_high; /* = null */

    /// Placement ID is optionally specified in the underline color.
    ::wisp::vt::Maybe<uint32_t> placement_id; /* = null */

    /// The row/col index for the image. These are 0-indexed. These
    /// are specified using diacritics. The row is first and the col
    /// is second. Both are optional. If not specified, they can continue
    /// a previous placement under certain conditions.
    ::wisp::vt::Maybe<uint32_t> row; /* = null */
    ::wisp::vt::Maybe<uint32_t> col; /* = null */

    /// The run width so far in cells.
    uint32_t width; /* = 1 */

    IncompletePlacement()
        : pin(), image_id_low(0), image_id_high(), placement_id(), row(), col(), width(1) {}

    /// Parse the incomplete placement information from a row and cell.
    ///
    /// The cell could be derived from the row but in our usage we already
    /// have the cell and we don't want to waste cycles recomputing it.
    static IncompletePlacement init(
        const Pin *row,
        const Cell *cell) {
        assert(cell->codepoint() == placeholder);
        const Style style = row->style(cell);

        IncompletePlacement result;
        result.pin = *row;
        result.image_id_low = colorToId(style.fg_color);
        {
            const uint32_t id = colorToId(style.underline_color);
            result.placement_id = id != 0 ? ::wisp::vt::Maybe<uint32_t>(id)
                                          : ::wisp::vt::Maybe<uint32_t>::none();
        }

        // Try to decode all our diacritics. Any invalid diacritics are
        // treated as if they don't exist. This isn't explicitly specified
        // at the time of writing this but it appears to be how Kitty behaves.
        size_t cps_len = 0;
        const uint32_t *cps = row->grapheme(cell, &cps_len);
        if (cps == nullptr) cps_len = 0;
        if (cps_len > 0) {
            /* Wisp: `orelse value: { log.warn(...); break :value null; }` is
             * just the null itself once the log is dropped. */
            result.row = getIndex(cps[0]);

            if (cps_len > 1) {
                result.col = getIndex(cps[1]);

                if (cps_len > 2) {
                    const ::wisp::vt::Maybe<uint32_t> high_ = getIndex(cps[2]);

                    if (high_.has) {
                        const uint32_t high = high_.value;
                        result.image_id_high =
                            high <= 0xFF ? ::wisp::vt::Maybe<uint8_t>((uint8_t)high)
                                         : ::wisp::vt::Maybe<uint8_t>::none();
                    }

                    // Any additional diacritics are ignored.
                }
            }
        }

        return result;
    }

    /// Append this incomplete placement to an existing placement to
    /// create a run. This returns true if the placements are compatible
    /// and were combined. If this returns false, the other placement is
    /// unchanged.
    bool append(const IncompletePlacement *other) {
        if (!canAppend(other)) return false;
        this->width += 1;
        return true;
    }

    bool canAppend(const IncompletePlacement *other) const {
        /* Wisp: upstream compares `?T` values directly; Zig's `==` on
         * optionals is true when both are null. These helpers spell that
         * out. */
        // Converted from Kitty's logic, don't @ me.
        /* Wisp: `self.col.?` — upstream relies on the invariant that a run
         * always has `col` set (PlacementIterator.next sets it to 0 before
         * starting a run), so the unwrap never trips. The assert records
         * the invariant; our Maybe value-initializes its payload, so a
         * NDEBUG build reads a defined 0 rather than indeterminate
         * memory. */
        assert(this->col.has);
        return this->image_id_low == other->image_id_low &&
               eqOptU32(this->placement_id, other->placement_id) &&
               (!other->row.has || eqOptU32(other->row, this->row)) &&
               (!other->col.has || other->col.value == this->col.value + this->width) &&
               (!other->image_id_high.has ||
                eqOptU8(other->image_id_high, this->image_id_high));
    }

    /// Complete the incomplete placement to create a full placement.
    /// This creates a new placement that isn't continuous with any previous
    /// placements.
    ///
    /// The pin is the pin of the cell that created this incomplete placement.
    Placement complete() const {
        Placement result;
        result.pin = this->pin;
        {
            const uint32_t low = this->image_id_low;
            const uint32_t high = this->image_id_high.has ? (uint32_t)this->image_id_high.value : 0;
            result.image_id = low | (high << 24);
        }
        result.placement_id = this->placement_id.orelse(0);
        result.col = this->col.orelse(0);
        result.row = this->row.orelse(0);
        result.width = this->width;
        result.height = 1;
        return result;
    }

    /// Convert a style color to a Kitty image protocol ID. This works by
    /// taking the 24 most significant bits of the color, which lets it work
    /// for both palette and rgb-based colors.
    static uint32_t colorToId(const ::wisp::vt::style::Style::Color &c) {
        switch (c.tag) {
        case ::wisp::vt::style::Style::Color::Tag::none: return 0;
        case ::wisp::vt::style::Style::Color::Tag::palette: return (uint32_t)c.palette;
        default: {
            const uint32_t r = (uint32_t)c.rgb.r;
            const uint32_t g = (uint32_t)c.rgb.g;
            const uint32_t b = (uint32_t)c.rgb.b;
            return (r << 16) | (g << 8) | b;
        }
        }
    }

    /* Wisp: Zig `?T == ?T`. */
    static bool eqOptU32(const ::wisp::vt::Maybe<uint32_t> &a,
                         const ::wisp::vt::Maybe<uint32_t> &b) {
        if (a.has != b.has) return false;
        if (!a.has) return true;
        return a.value == b.value;
    }

    static bool eqOptU8(const ::wisp::vt::Maybe<uint8_t> &a,
                        const ::wisp::vt::Maybe<uint8_t> &b) {
        if (a.has != b.has) return false;
        if (!a.has) return true;
        return a.value == b.value;
    }
};

/// Iterator over unicode virtual placements.
struct PlacementIterator {
    PageList::RowIterator row_it;
    ::wisp::vt::Maybe<Pin> row;

    PlacementIterator() : row_it(), row() {}

    /* Wisp: `?Placement next()` -> `bool next(Placement *out)`. */
    bool next(Placement *out) {
        while (this->row.has) {
            /* Wisp: `while (self.row) |*row|` captures a POINTER into
             * self.row, and the body mutates `row.x` to communicate the
             * resume position back to the iterator (see the assert
             * below). */
            Pin *row = &this->row.value;

            // This row flag is set on rows that have the virtual placeholder
            if (!row->rowAndCell().row->kitty_virtual_placeholder()) {
                Pin p;
                this->row = this->row_it.next(&p) ? ::wisp::vt::Maybe<Pin>(p)
                                                  : ::wisp::vt::Maybe<Pin>::none();
                continue;
            }

            // Our current run. A run is always only a single row. This
            // assumption is built-in to our logic so if we want to change
            // this later we have to redo the logic; tests should cover;
            ::wisp::vt::Maybe<IncompletePlacement> run; /* = null */

            // Iterate over our remaining cells and find one with a placeholder.
            size_t cells_len = 0;
            Cell *cells = row->cells(Pin::CellSubset::right, &cells_len);
            /* Wisp: `for (cells, row.x..)` evaluates both the slice and the
             * range start ONCE, before the body starts mutating `row.x`.
             * Snapshot them. */
            const CellCountInt start_x = row->x;
            for (size_t i = 0; i < cells_len; i++) {
                Cell *cell = &cells[i];
                const size_t x = (size_t)start_x + i;

                // "row" now points to the top-left pin of the placement.
                // We need this temporary state to build our incomplete
                // placement.
                /* Wisp: upstream asserts `@intFromPtr(row) ==
                 * @intFromPtr(&self.row)`; a Zig optional's payload shares
                 * the address of the optional, while our Maybe puts `has`
                 * first, so the port compares against the payload. */
                assert((uintptr_t)row == (uintptr_t)&this->row.value);
                row->x = (CellCountInt)x;

                // If this cell doesn't have the placeholder, then we
                // complete the run if we have it otherwise we just move
                // on and keep searching.
                if (cell->codepoint() != placeholder) {
                    if (run.has) {
                        *out = run.value.complete();
                        return true;
                    }
                    continue;
                }

                // If we don't have a previous run, then we save this
                // incomplete one, start a run, and move on.
                const IncompletePlacement curr = IncompletePlacement::init(row, cell);
                if (run.has) {
                    IncompletePlacement *prev = &run.value;

                    // If we can't append, then we complete the previous
                    // run and return it.
                    if (!prev->append(&curr)) {
                        // Note: self.row is already updated due to the
                        // row pointer above. It points back at this same
                        // cell so we can continue the new placements from
                        // here.
                        *out = prev->complete();
                        return true;
                    }

                    // append is mutating so if we reached this point
                    // then prev has been updated.
                } else {
                    // For appending, we need to set our initial values.
                    IncompletePlacement prev = curr;
                    if (!prev.row.has) prev.row = ::wisp::vt::Maybe<uint32_t>(0);
                    if (!prev.col.has) prev.col = ::wisp::vt::Maybe<uint32_t>(0);
                    run = ::wisp::vt::Maybe<IncompletePlacement>(prev);
                }
            }

            // We move to the next row no matter what
            {
                Pin p;
                this->row = this->row_it.next(&p) ? ::wisp::vt::Maybe<Pin>(p)
                                                  : ::wisp::vt::Maybe<Pin>::none();
            }

            // If we have a run, we complete it here.
            if (run.has) {
                *out = run.value.complete();
                return true;
            }
        }

        return false;
    }
};

/// Returns an iterator that iterates over all of the virtual placements
/// in the given pin. If `limit` is provided, the iterator will stop
/// when it reaches that pin (inclusive). If `limit` is not provided,
/// the iterator will continue until the end of the page list.
/* Wisp: defined after PlacementIterator because C++ needs the complete
 * type to return it by value; upstream's order is preserved otherwise. */
inline PlacementIterator placementIterator(
    const Pin &pin,
    ::wisp::vt::Maybe<Pin> limit) {
    PageList::RowIterator row_it = pin.rowIterator(PageList::Direction::right_down, limit);
    ::wisp::vt::Maybe<Pin> row;
    {
        Pin p;
        if (row_it.next(&p)) row = ::wisp::vt::Maybe<Pin>(p);
    }
    PlacementIterator result;
    result.row_it = row_it;
    result.row = row;
    return result;
}

/// These are the diacritics used with the Kitty graphics protocol
/// Unicode placement feature to specify the row/column for placement.
/// The index into the array determines the value.
///
/// This is derived from:
/// https://sw.kovidgoyal.net/kitty/_downloads/f0a0de9ec8d9ff4456206db8e0814937/rowcolumn-diacritics.txt
static const uint32_t diacritics[] = {
    0x0305,
    0x030D,
    0x030E,
    0x0310,
    0x0312,
    0x033D,
    0x033E,
    0x033F,
    0x0346,
    0x034A,
    0x034B,
    0x034C,
    0x0350,
    0x0351,
    0x0352,
    0x0357,
    0x035B,
    0x0363,
    0x0364,
    0x0365,
    0x0366,
    0x0367,
    0x0368,
    0x0369,
    0x036A,
    0x036B,
    0x036C,
    0x036D,
    0x036E,
    0x036F,
    0x0483,
    0x0484,
    0x0485,
    0x0486,
    0x0487,
    0x0592,
    0x0593,
    0x0594,
    0x0595,
    0x0597,
    0x0598,
    0x0599,
    0x059C,
    0x059D,
    0x059E,
    0x059F,
    0x05A0,
    0x05A1,
    0x05A8,
    0x05A9,
    0x05AB,
    0x05AC,
    0x05AF,
    0x05C4,
    0x0610,
    0x0611,
    0x0612,
    0x0613,
    0x0614,
    0x0615,
    0x0616,
    0x0617,
    0x0657,
    0x0658,
    0x0659,
    0x065A,
    0x065B,
    0x065D,
    0x065E,
    0x06D6,
    0x06D7,
    0x06D8,
    0x06D9,
    0x06DA,
    0x06DB,
    0x06DC,
    0x06DF,
    0x06E0,
    0x06E1,
    0x06E2,
    0x06E4,
    0x06E7,
    0x06E8,
    0x06EB,
    0x06EC,
    0x0730,
    0x0732,
    0x0733,
    0x0735,
    0x0736,
    0x073A,
    0x073D,
    0x073F,
    0x0740,
    0x0741,
    0x0743,
    0x0745,
    0x0747,
    0x0749,
    0x074A,
    0x07EB,
    0x07EC,
    0x07ED,
    0x07EE,
    0x07EF,
    0x07F0,
    0x07F1,
    0x07F3,
    0x0816,
    0x0817,
    0x0818,
    0x0819,
    0x081B,
    0x081C,
    0x081D,
    0x081E,
    0x081F,
    0x0820,
    0x0821,
    0x0822,
    0x0823,
    0x0825,
    0x0826,
    0x0827,
    0x0829,
    0x082A,
    0x082B,
    0x082C,
    0x082D,
    0x0951,
    0x0953,
    0x0954,
    0x0F82,
    0x0F83,
    0x0F86,
    0x0F87,
    0x135D,
    0x135E,
    0x135F,
    0x17DD,
    0x193A,
    0x1A17,
    0x1A75,
    0x1A76,
    0x1A77,
    0x1A78,
    0x1A79,
    0x1A7A,
    0x1A7B,
    0x1A7C,
    0x1B6B,
    0x1B6D,
    0x1B6E,
    0x1B6F,
    0x1B70,
    0x1B71,
    0x1B72,
    0x1B73,
    0x1CD0,
    0x1CD1,
    0x1CD2,
    0x1CDA,
    0x1CDB,
    0x1CE0,
    0x1DC0,
    0x1DC1,
    0x1DC3,
    0x1DC4,
    0x1DC5,
    0x1DC6,
    0x1DC7,
    0x1DC8,
    0x1DC9,
    0x1DCB,
    0x1DCC,
    0x1DD1,
    0x1DD2,
    0x1DD3,
    0x1DD4,
    0x1DD5,
    0x1DD6,
    0x1DD7,
    0x1DD8,
    0x1DD9,
    0x1DDA,
    0x1DDB,
    0x1DDC,
    0x1DDD,
    0x1DDE,
    0x1DDF,
    0x1DE0,
    0x1DE1,
    0x1DE2,
    0x1DE3,
    0x1DE4,
    0x1DE5,
    0x1DE6,
    0x1DFE,
    0x20D0,
    0x20D1,
    0x20D4,
    0x20D5,
    0x20D6,
    0x20D7,
    0x20DB,
    0x20DC,
    0x20E1,
    0x20E7,
    0x20E9,
    0x20F0,
    0x2CEF,
    0x2CF0,
    0x2CF1,
    0x2DE0,
    0x2DE1,
    0x2DE2,
    0x2DE3,
    0x2DE4,
    0x2DE5,
    0x2DE6,
    0x2DE7,
    0x2DE8,
    0x2DE9,
    0x2DEA,
    0x2DEB,
    0x2DEC,
    0x2DED,
    0x2DEE,
    0x2DEF,
    0x2DF0,
    0x2DF1,
    0x2DF2,
    0x2DF3,
    0x2DF4,
    0x2DF5,
    0x2DF6,
    0x2DF7,
    0x2DF8,
    0x2DF9,
    0x2DFA,
    0x2DFB,
    0x2DFC,
    0x2DFD,
    0x2DFE,
    0x2DFF,
    0xA66F,
    0xA67C,
    0xA67D,
    0xA6F0,
    0xA6F1,
    0xA8E0,
    0xA8E1,
    0xA8E2,
    0xA8E3,
    0xA8E4,
    0xA8E5,
    0xA8E6,
    0xA8E7,
    0xA8E8,
    0xA8E9,
    0xA8EA,
    0xA8EB,
    0xA8EC,
    0xA8ED,
    0xA8EE,
    0xA8EF,
    0xA8F0,
    0xA8F1,
    0xAAB0,
    0xAAB2,
    0xAAB3,
    0xAAB7,
    0xAAB8,
    0xAABE,
    0xAABF,
    0xAAC1,
    0xFE20,
    0xFE21,
    0xFE22,
    0xFE23,
    0xFE24,
    0xFE25,
    0xFE26,
    0x10A0F,
    0x10A38,
    0x1D185,
    0x1D186,
    0x1D187,
    0x1D188,
    0x1D189,
    0x1D1AA,
    0x1D1AB,
    0x1D1AC,
    0x1D1AD,
    0x1D242,
    0x1D243,
    0x1D244,
};

/* Wisp: the Zig slice's length. */
static const size_t diacritics_len = sizeof(diacritics) / sizeof(diacritics[0]);

/// Get the row/col index for a diacritic codepoint. These are 0-indexed.
/* Wisp: std.sort.binarySearch with an order-returning comparator is an
 * exact-match binary search returning the index, or null. */
inline ::wisp::vt::Maybe<uint32_t> getIndex(uint32_t cp) {
    size_t low = 0;
    size_t high = diacritics_len;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (cp == diacritics[mid]) return ::wisp::vt::Maybe<uint32_t>((uint32_t)mid);
        if (cp < diacritics[mid]) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return ::wisp::vt::Maybe<uint32_t>::none();
}

} /* namespace graphics_unicode */
} /* namespace graphics */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_UNICODE_HPP */
