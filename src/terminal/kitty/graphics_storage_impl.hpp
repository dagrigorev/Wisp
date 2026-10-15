/* Transliterated from Ghostty src/terminal/kitty/graphics_storage.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: out-of-line definitions for graphics_storage.hpp. `Screen` carries
 * `ImageStorage` by value, so graphics_storage.hpp is included *by*
 * screen.hpp and can only forward-declare `Screen` and `Terminal`; every
 * body that dereferences one therefore lives here or in
 * graphics_storage_screen_impl.hpp. Nothing was rewritten in the move: the
 * bodies, their comments and their order are graphics_storage.zig's.
 *
 * This file holds the bodies that need `Terminal`. terminal.hpp includes it
 * at its end, where both `Screen` and `Terminal` are complete. Including it
 * directly pulls in the whole subsystem, so this is the header a consumer
 * of Kitty graphics storage includes.
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_IMPL_HPP
#define WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_IMPL_HPP

#include "../../vt/terminal.hpp"
#include "graphics_storage.hpp"
#include "graphics_storage_screen_impl.hpp"

namespace wisp {
namespace terminal {
namespace kitty {
namespace graphics_storage {

inline ImageStorage::Placement::CellOffset
ImageStorage::Placement::cellOffset(const Terminal *t) const {
    const uint32_t cell_width = t->width_px / t->cols;
    const uint32_t cell_height = t->height_px / t->rows;

    CellOffset r;
    r.x = cell_width > 0 ? (x_offset < cell_width - 1 ? x_offset : cell_width - 1) : 0;
    r.y = cell_height > 0 ? (y_offset < cell_height - 1 ? y_offset : cell_height - 1) : 0;
    return r;
}

inline ImageStorage::Placement::PixelSize
ImageStorage::Placement::pixelSize(const Image &image, const Terminal *t) const {
    const SourceRect source = sourceRect(image);
    const uint32_t width = source.width;
    const uint32_t height = source.height;

    PixelSize out;

    /* If we don't have any specified cols or rows then the placement
     * should be the native size of the image, and doesn't need to be
     * re-scaled. */
    if (columns == 0 && rows == 0) {
        out.width = width;
        out.height = height;
        return out;
    }

    /* We calculate the size of a cell so that we can multiply
     * it by the specified cols/rows to get the correct px size.
     *
     * We assume that the width is divided evenly by the column
     * count and the height by the row count, because it should be. */
    const uint32_t cell_width = t->width_px / t->cols;
    const uint32_t cell_height = t->height_px / t->rows;
    const CellOffset cell_offset = cellOffset(t);

    /* If we have a specified cols AND rows then we calculate
     * the width and height from them directly, we don't need
     * to adjust for aspect ratio. */
    if (columns > 0 && rows > 0) {
        const uint32_t calc_width = saturatingMul(cell_width, columns);
        const uint32_t calc_height = saturatingMul(cell_height, rows);

        out.width = satSubU32(calc_width, cell_offset.x);
        out.height = satSubU32(calc_height, cell_offset.y);
        return out;
    }

    /* Either the columns or the rows were specified, but not both,
     * so we need to calculate the other one based on the aspect ratio. */

    /* If only the columns were specified, we determine
     * the height of the image based on the aspect ratio. */
    if (columns > 0) {
        const uint32_t calc_width =
            satSubU32(saturatingMul(cell_width, columns), cell_offset.x);
        const uint32_t calc_height = scaleDimension(calc_width, height, width);

        out.width = calc_width;
        out.height = calc_height;
        return out;
    }

    /* Otherwise, only the rows were specified, so we
     * determine the width based on the aspect ratio. */
    {
        const uint32_t calc_height =
            satSubU32(saturatingMul(cell_height, rows), cell_offset.y);
        const uint32_t calc_width = scaleDimension(calc_height, width, height);

        out.width = calc_width;
        out.height = calc_height;
        return out;
    }
}

inline ImageStorage::Placement::GridSize
ImageStorage::Placement::gridSize(const Image &image, const Terminal *t) const {
    GridSize out;

    /* If we have a specified columns and rows then this is trivial. */
    if (columns > 0 && rows > 0) {
        out.cols = columns;
        out.rows = rows;
        return out;
    }

    /* Otherwise we calculate the pixel size, divide by
     * cell size, and round up to the nearest integer. */
    const PixelSize calc_size = pixelSize(image, t);
    const CellOffset cell_offset = cellOffset(t);
    const uint32_t cell_width = t->width_px / t->cols;
    const uint32_t cell_height = t->height_px / t->rows;
    /* Wisp: `std.math.divCeil(u32, a, b) catch 0`. The numerator can
     * be maxInt(u32) after saturation, so the +b-1 is done in u64. */
    out.cols = cell_width == 0
                   ? 0
                   : (uint32_t)(((uint64_t)satAddU32(calc_size.width, cell_offset.x) +
                                 (uint64_t)cell_width - 1) /
                                (uint64_t)cell_width);
    out.rows = cell_height == 0
                   ? 0
                   : (uint32_t)(((uint64_t)satAddU32(calc_size.height, cell_offset.y) +
                                 (uint64_t)cell_height - 1) /
                                (uint64_t)cell_height);
    return out;
    /* NOTE: Above `divCeil`s can only fail if the cell size is 0,
     *       in such a case it seems safe to return 0 for this. */
}

inline bool ImageStorage::Placement::clipTop(const Image &image, const Terminal *t,
                                             uint32_t count, uint32_t span) {
    assert(count > 0 && count < span);
    const SourceRect src = sourceRect(image);
    const uint32_t crop =
        rows > 0
            /* Scaled placement: each grid row shows an equal
             * fraction of the source. */
            ? (uint32_t)((uint64_t)src.height * (uint64_t)count / (uint64_t)span)
            /* Native size: each grid row shows one cell height of
             * source pixels (kitty does the same). */
            : saturatingMul(t->height_px / t->rows, count);
    if (crop >= src.height) return false;
    source_x = src.x;
    source_y = src.y + crop;
    source_width = src.width;
    source_height = src.height - crop;
    if (rows > 0) rows -= count;
    return true;
}

inline bool ImageStorage::Placement::clipBottom(const Image &image, const Terminal *t,
                                                uint32_t count, uint32_t span) {
    assert(count > 0 && count < span);
    const SourceRect src = sourceRect(image);
    /* An empty source can never be clipped smaller. This also
     * guarantees we never store an explicit zero source height,
     * which would be reinterpreted as "full image". */
    if (src.height == 0) return false;
    if (rows > 0) {
        const uint32_t crop =
            (uint32_t)((uint64_t)src.height * (uint64_t)count / (uint64_t)span);
        if (crop >= src.height) return false;
        source_height = src.height - crop;
        rows -= count;
    } else {
        /* The remaining rows can show this many source pixels,
         * with the first cell losing cellOffset pixels to the
         * placement's y offset. */
        const uint32_t visible = saturatingMul(t->height_px / t->rows, span - count);
        const uint32_t offset = cellOffset(t).y;
        if (visible <= offset) return false;
        const uint32_t avail = visible - offset;
        source_height = src.height < avail ? src.height : avail;
    }
    source_x = src.x;
    source_y = src.y;
    source_width = src.width;
    return true;
}

inline bool ImageStorage::Placement::rect(const Image &image, const Terminal *t, Rect *out) const {
    const GridSize grid_size = gridSize(image, t);
    /* Wisp: value-initialized; upstream's switch covers every tag
     * and the `virtual`/`relative` arms return. */
    PageList::Pin *pin = nullptr;
    switch (location.tag) {
    case Location::Tag::pin: pin = location.pin; break;
    case Location::Tag::virtual_:
    case Location::Tag::relative: return false;
    default: return false;
    }
    if (pin->garbage) return false;

    /* A zero pixel-sized placement can produce a zero grid size when
     * pixel geometry is unavailable. It occupies no rectangle. */
    if (grid_size.cols == 0 || grid_size.rows == 0) return false;

    const PageList::Pin::Overflow o = pin->downOverflow(grid_size.rows - 1);
    PageList::Pin br =
        o.tag == PageList::Pin::Overflow::Tag::offset ? o.offset : o.overflow.end;
    {
        /* We need to sub one here because the x value is
         * one width already. So if the image is width "1"
         * then we add zero to X because X itself is width 1. */
        const uint32_t extended = satAddU32((uint32_t)pin->x, grid_size.cols - 1);
        const uint32_t limit = (uint32_t)t->cols - 1;
        br.x = (vtsize::CellCountInt)(extended < limit ? extended : limit);
    }

    out->top_left = *pin;
    out->bottom_right = br;
    return true;
}

inline ImageStorage::ScrollMargins ImageStorage::scrollMarginsBegin(Terminal *t, int64_t delta,
                                                                    ScrollOp op) {
    Screen *s = t->screens.active;
    assert(this == &s->kitty_images);
    ScrollMargins result;
    result.screen = s;

    const int64_t top = (int64_t)t->scrolling_region.top;
    const int64_t bottom = (int64_t)t->scrolling_region.bottom;

    bool mutated = false;
    PlacementMap::Iterator it = placements.iterator();
    PlacementMap::Entry entry;
    while (it.next(&entry)) {
        Placement *p = entry.value_ptr;

        /* Virtual placements follow their placeholder cells and
         * relative placements follow their parents, so neither is
         * ever adjusted by scrolls, matching kitty. */
        if (p->location.tag != Placement::Location::Tag::pin) continue;
        PageList::Pin *pin = p->location.pin;

        /* Pruned placements are reaped by reapGarbagePlacements. */
        if (pin->garbage) continue;

        /* Placements anchored outside the active area (scrollback)
         * are never touched by a scroll within the margins. */
        const Maybe<point::Point> pt = s->pages.pointFromPin(point::Tag::active, *pin);
        if (!pt.has) continue;
        const point::Coordinate coord = pt.value.coord();
        const int64_t y = (int64_t)coord.y;

        /* An in-place operation doesn't touch pins anchored outside
         * the region rows, and a placement anchored there can never
         * be entirely inside the region, so it needs no work at all. */
        if (op == ScrollOp::in_place && (y < top || y > bottom)) continue;

        /* The final position defaults to unmoved; restoring the pin
         * to this exact spot afterwards is what stops the row
         * operations from dragging the placement around. */
        int64_t final_y = y;

        /* Wisp: upstream's `inside:` labeled block. `removed` carries
         * the `continue` that targets the outer loop. */
        bool removed = false;
        do {
            Image image;
            if (!images.get(entry.key_ptr->image_id, &image)) break;
            const Placement::GridSize grid = p->gridSize(image, t);

            /* Unknown geometry (e.g. pixel sizes not yet reported)
             * means we can't know the extent, so we can't consider
             * the placement contained. */
            if (grid.rows == 0 || grid.cols == 0) break;

            /* Only placements entirely within the scrolling region
             * move. Kitty only has top/bottom margins so it only
             * checks rows; the column check is our generalization
             * for left/right margins and is trivially true for
             * full-width regions. */
            if (y < top || y + (int64_t)grid.rows - 1 > bottom) break;
            const int64_t x = (int64_t)coord.x;
            {
                const int64_t right_edge = x + (int64_t)grid.cols - 1;
                const int64_t cols_limit = (int64_t)t->cols - 1;
                const int64_t clamped =
                    right_edge < cols_limit ? right_edge : cols_limit;
                if (x < (int64_t)t->scrolling_region.left ||
                    clamped > (int64_t)t->scrolling_region.right)
                    break;
            }

            const int64_t new_y = y + delta;
            const int64_t rows = (int64_t)grid.rows;
            const int64_t top_clip = (top - new_y) > 0 ? (top - new_y) : 0;
            const int64_t bottom_clip_raw = new_y + rows - 1 - bottom;
            const int64_t bottom_clip = bottom_clip_raw > 0 ? bottom_clip_raw : 0;
            /* Wisp: upstream's `clip:` labeled block. `visible` is
             * assigned on every path, so it is never read unset. */
            bool visible;
            do {
                if (top_clip >= rows || bottom_clip >= rows) {
                    visible = false;
                    break;
                }

                if (top_clip > 0) {
                    if (!p->clipTop(image, t, (uint32_t)top_clip, grid.rows)) {
                        visible = false;
                        break;
                    }
                    mutated = true;
                    final_y = top;
                    visible = true;
                    break;
                }

                if (bottom_clip > 0) {
                    if (!p->clipBottom(image, t, (uint32_t)bottom_clip, grid.rows)) {
                        visible = false;
                        break;
                    }
                    mutated = true;
                }

                final_y = new_y;
                visible = true;
            } while (0);

            /* Scrolled or clipped entirely out of the region: the
             * placement is deleted, like kitty. The image itself is
             * retained for future placements. */
            if (!visible) {
                removePlacement(s, entry);
                mutated = true;
                removed = true;
            }
        } while (0);
        if (removed) continue;

        ScrollMargins::Restore restore;
        restore.pin = pin;
        restore.x = pin->x;
        restore.y = (uint32_t)final_y;
        if (!result.restores.append(s->alloc, restore)) {
            /* OOM: this placement is left to the whims of the
             * generic pin tracking. Degraded, but safe. */
            /* log.warn("OOM adjusting image placement for scroll") */
            continue;
        }
    }

    if (mutated) {
        /* Placements deleted by clipping may orphan relative placements.
         * Orphans have no pins so this never touches the restores. */
        (void)removeOrphans(s, nullptr);
        markMutated();
    }

    return result;
}

inline void ImageStorage::clearScreen(Allocator alloc, Terminal *t) {
    const size_t placements_before = placements.count();
    const size_t images_before = images.count();

    /* Delete unused placements and images. Orphaned relative
     * placements must be reaped before the image sweep so that
     * their images are reclaimable too. */
    deleteVisiblePlacements(alloc, t, true);
    (void)removeOrphans(t->screens.active, nullptr);
    ImageMap::Iterator image_it = images.iterator();
    ImageMap::Entry entry;
    while (image_it.next(&entry)) {
        deleteIfUnused(alloc, *entry.key_ptr);
    }

    /* Mark mutated only if things changed. */
    if (placements.count() != placements_before || images.count() != images_before)
        markMutated();
}

inline void ImageStorage::delete_(Allocator alloc, Terminal *t,
                                  const command::Delete::Action &cmd) {
    /* Deletes only ever remove placements/images, so comparing counts
     * before and after tells us whether anything actually changed.
     * Only then do we mark a mutation, so a delete that matches nothing
     * doesn't dirty the image state or bump the generation.
     *
     * Wisp: upstream's `defer` is this RAII guard; it runs after the
     * removeOrphans call at the bottom, like Zig's. */
    struct MutationGuard {
        ImageStorage *self;
        size_t placements_before;
        size_t images_before;

        MutationGuard(ImageStorage *s)
            : self(s), placements_before(s->placements.count()),
              images_before(s->images.count()) {}
        ~MutationGuard() {
            if (self->placements.count() != placements_before ||
                self->images.count() != images_before)
                self->markMutated();
        }
    } mutation_guard(this);

    typedef command::Delete::Action Action;
    switch (cmd.tag) {
    case Action::Tag::all: deleteVisiblePlacements(alloc, t, cmd.all); break;

    case Action::Tag::id:
        deleteById(alloc, t->screens.active, cmd.id.image_id, cmd.id.placement_id,
                   cmd.id.delete_);
        break;

    case Action::Tag::newest: {
        /* Wisp: upstream's `newest:` labeled block. */
        const Maybe<Image> img = imageByNumber(cmd.newest.image_number);
        if (!img.has) break;
        deleteById(alloc, t->screens.active, img.value.id, cmd.newest.placement_id,
                   cmd.newest.delete_);
        break;
    }

    case Action::Tag::intersect_cursor: {
        deleteIntersecting(alloc, t,
                           point::Point::active(t->screens.active->cursor.x,
                                                t->screens.active->cursor.y),
                           cmd.intersect_cursor, 0, nullptr);
        break;
    }

    case Action::Tag::intersect_cell: {
        /* Wisp: upstream's `intersect_cell:` labeled block. */
        if (cmd.intersect_cell.x <= 0 || cmd.intersect_cell.y <= 0) {
            /* log.warn("delete intersect cell coords must be at least 1") */
            break;
        }

        const uint32_t cx = cmd.intersect_cell.x - 1;
        const uint32_t cy = cmd.intersect_cell.y - 1;
        if (cx > 0xFFFFu) break; /* std.math.cast(size.CellCountInt, ...) */
        if (cy > 0xFFFFu) break;
        deleteIntersecting(alloc, t,
                           point::Point::active((vtsize::CellCountInt)cx, cy),
                           cmd.intersect_cell.delete_, 0, nullptr);
        break;
    }

    case Action::Tag::intersect_cell_z: {
        /* Wisp: upstream's `intersect_cell_z:` labeled block. */
        if (cmd.intersect_cell_z.x <= 0 || cmd.intersect_cell_z.y <= 0) {
            /* log.warn("delete intersect cell coords must be at least 1") */
            break;
        }

        const uint32_t cx = cmd.intersect_cell_z.x - 1;
        const uint32_t cy = cmd.intersect_cell_z.y - 1;
        if (cx > 0xFFFFu) break;
        if (cy > 0xFFFFu) break;
        deleteIntersecting(alloc, t,
                           point::Point::active((vtsize::CellCountInt)cx, cy),
                           cmd.intersect_cell_z.delete_, cmd.intersect_cell_z.z,
                           &zFilter);
        break;
    }

    case Action::Tag::column: {
        /* Wisp: upstream's `column:` labeled block. */
        if (cmd.column.x <= 0) {
            /* log.warn("delete column must be greater than zero") */
            break;
        }

        const uint32_t x = cmd.column.x - 1;
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            const Maybe<Image> img = imageById(entry.key_ptr->image_id);
            if (!img.has) continue;
            Rect r;
            if (!entry.value_ptr->rect(img.value, t, &r)) continue;
            if ((uint32_t)r.top_left.x <= x && (uint32_t)r.bottom_right.x >= x) {
                removePlacement(t->screens.active, entry);
                if (cmd.column.delete_) deleteIfUnused(alloc, img.value.id);
            }
        }
        break;
    }

    case Action::Tag::row: {
        /* Wisp: upstream's `row:` labeled block. */
        if (cmd.row.y <= 0) {
            /* log.warn("delete row must be greater than zero") */
            break;
        }

        /* v.y is in active coords so we want to convert it to a pin
         * so we can compare by page offsets. */
        const uint32_t ry = cmd.row.y - 1;
        if (ry > 0xFFFFu) break; /* std.math.cast(size.CellCountInt, v.y - 1) */
        const Maybe<PageList::Pin> target_pin =
            t->screens.active->pages.pin(point::Point::active(0, ry));
        if (!target_pin.has) break;

        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            const Maybe<Image> img = imageById(entry.key_ptr->image_id);
            if (!img.has) continue;
            Rect r;
            if (!entry.value_ptr->rect(img.value, t, &r)) continue;

            /* We need to copy our pin to ensure we are at least at
             * the top-left x. */
            PageList::Pin target_pin_copy = target_pin.value;
            target_pin_copy.x = r.top_left.x;
            if (target_pin_copy.isBetween(r.top_left, r.bottom_right)) {
                removePlacement(t->screens.active, entry);
                if (cmd.row.delete_) deleteIfUnused(alloc, img.value.id);
            }
        }
        break;
    }

    case Action::Tag::z: {
        PlacementMap::Iterator it = placements.iterator();
        PlacementMap::Entry entry;
        while (it.next(&entry)) {
            switch (entry.value_ptr->location.tag) {
            /* Relative placements carry their own z value and
             * are matched by z deletes just like kitty does. */
            case Placement::Location::Tag::pin:
            case Placement::Location::Tag::relative: break;

            /* Virtual placeholders cannot delete by z according
             * to the spec. */
            case Placement::Location::Tag::virtual_: continue;
            }

            if (entry.value_ptr->z == cmd.z.z) {
                const uint32_t image_id = entry.key_ptr->image_id;
                removePlacement(t->screens.active, entry);
                if (cmd.z.delete_) deleteIfUnused(alloc, image_id);
            }
        }
        break;
    }

    case Action::Tag::range: {
        /* Wisp: upstream's `range:` labeled block.
         * Both bounds default to zero when omitted. An inverted range
         * or a zero upper bound is not an error, it just selects
         * nothing: image IDs are always non-zero. */
        if (cmd.range.last == 0 || cmd.range.first > cmd.range.last) break;

        /* Remove matching placements in one pass. */
        {
            PlacementMap::Iterator placement_it = placements.iterator();
            PlacementMap::Entry entry;
            while (placement_it.next(&entry)) {
                if (entry.key_ptr->image_id < cmd.range.first ||
                    entry.key_ptr->image_id > cmd.range.last)
                    continue;

                removePlacement(t->screens.active, entry);
            }
        }

        /* Uppercase deletion also frees matching images that are now
         * unused, including images that had no placements initially. */
        if (!cmd.range.delete_) break;
        {
            ImageMap::Iterator image_it = images.iterator();
            ImageMap::Entry entry;
            while (image_it.next(&entry)) {
                const uint32_t image_id = *entry.key_ptr;
                if (image_id >= cmd.range.first && image_id <= cmd.range.last) {
                    deleteIfUnused(alloc, image_id);
                }
            }
        }
        break;
    }

    case Action::Tag::animation_frames:
        deleteAnimationFrame(alloc, t->screens.active, cmd.animation_frames);
        break;
    }

    /* Deleting placements orphans any relative placements parented
     * to them (transitively). Their lifetime is tied to the parent
     * so they are removed as well, and an uppercase delete also
     * frees any image the cascade leaves without placements (the
     * per-branch deleteIfUnused calls above ran while the orphans
     * still counted as placements). */
    bool delete_unused = false;
    switch (cmd.tag) {
    case Action::Tag::all: delete_unused = cmd.all; break;
    case Action::Tag::intersect_cursor: delete_unused = cmd.intersect_cursor; break;
    case Action::Tag::id: delete_unused = cmd.id.delete_; break;
    case Action::Tag::newest: delete_unused = cmd.newest.delete_; break;
    case Action::Tag::animation_frames:
        delete_unused = cmd.animation_frames.delete_;
        break;
    case Action::Tag::intersect_cell: delete_unused = cmd.intersect_cell.delete_; break;
    case Action::Tag::intersect_cell_z:
        delete_unused = cmd.intersect_cell_z.delete_;
        break;
    case Action::Tag::range: delete_unused = cmd.range.delete_; break;
    case Action::Tag::column: delete_unused = cmd.column.delete_; break;
    case Action::Tag::row: delete_unused = cmd.row.delete_; break;
    case Action::Tag::z: delete_unused = cmd.z.delete_; break;
    }
    (void)removeOrphans(t->screens.active, delete_unused ? &alloc : nullptr);
}

inline void ImageStorage::deleteVisiblePlacements(Allocator alloc, Terminal *t,
                                                  bool delete_unused) {
    PlacementMap::Iterator it = placements.iterator();
    PlacementMap::Entry entry;
    while (it.next(&entry)) {
        /* Virtual placements are never selected by visible
         * deletes per the protocol. Relative placements are
         * removed with their parents instead (removeOrphans). */
        if (entry.value_ptr->location.tag != Placement::Location::Tag::pin) continue;
        PageList::Pin *pin = entry.value_ptr->location.pin;
        if (pin->garbage) continue;

        /* Placements anchored in the active area are necessarily visible.
         * Only compute their extent when the anchor is already in history
         * and the bottom edge may still intersect the active area. */
        if (!t->screens.active->pages.pointFromPin(point::Tag::active, *pin).has) {
            const Maybe<Image> image = imageById(entry.key_ptr->image_id);
            if (!image.has) continue;
            Rect r;
            if (!entry.value_ptr->rect(image.value, t, &r)) continue;
            if (!t->screens.active->pages
                     .pointFromPin(point::Tag::active, r.bottom_right)
                     .has)
                continue;
        }

        const uint32_t image_id = entry.key_ptr->image_id;
        removePlacement(t->screens.active, entry);
        if (delete_unused) deleteIfUnused(alloc, image_id);
    }
}

inline void ImageStorage::deleteIntersecting(Allocator alloc, Terminal *t, const point::Point &p,
                        bool delete_unused, int32_t filter_ctx,
                        bool (*filter)(int32_t, const Placement &)) {
    /* Convert our target point to a pin for comparison. */
    const Maybe<PageList::Pin> target_pin = t->screens.active->pages.pin(p);
    if (!target_pin.has) return;

    PlacementMap::Iterator it = placements.iterator();
    PlacementMap::Entry entry;
    while (it.next(&entry)) {
        const Maybe<Image> img = imageById(entry.key_ptr->image_id);
        if (!img.has) continue;
        Rect r;
        if (!entry.value_ptr->rect(img.value, t, &r)) continue;
        if (r.contains(target_pin.value)) {
            if (filter != nullptr && !filter(filter_ctx, *entry.value_ptr)) continue;
            removePlacement(t->screens.active, entry);
            if (delete_unused) deleteIfUnused(alloc, img.value.id);
        }
    }
}

} /* namespace graphics_storage */
} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_GRAPHICS_STORAGE_IMPL_HPP */
