/* Transliterated from Ghostty src/terminal/Screen.zig and Selection.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Out-of-line definitions for screen.hpp (included from its end).
 * Formatter-backed functions (dumpString, selectionString) are defined at
 * the end of formatter.hpp.
 */

#pragma once
#ifndef WISP_VT_SCREEN_IMPL_HPP
#define WISP_VT_SCREEN_IMPL_HPP

namespace wisp {
namespace vt {

/* ====================================================================== */
/* Selection.zig                                                          */

inline void Selection::deinit(Screen *s) const {
    switch (bounds.tag) {
    case Bounds::Tag::tracked:
        s->pages.untrackPin(bounds.tracked.start);
        s->pages.untrackPin(bounds.tracked.end);
        break;

    case Bounds::Tag::untracked: break;
    }
}

inline bool Selection::track(Screen *s, Selection *out) const {
    assert(!tracked());

    /* Track our pins */
    const Pin start_pin = bounds.untracked.start;
    const Pin end_pin = bounds.untracked.end;
    Pin *tracked_start = s->pages.trackPin(start_pin);
    if (!tracked_start) return false;
    Pin *tracked_end = s->pages.trackPin(end_pin);
    if (!tracked_end) {
        s->pages.untrackPin(tracked_start);
        return false;
    }

    Selection r;
    r.bounds.tag = Bounds::Tag::tracked;
    r.bounds.tracked.start = tracked_start;
    r.bounds.tracked.end = tracked_end;
    r.rectangle = rectangle;
    *out = r;
    return true;
}

inline Selection::Pin Selection::topLeft(const Screen *s) const {
    switch (order(s)) {
    case Order::forward: return start();
    case Order::reverse: return end();
    case Order::mirrored_forward: {
        Pin p = start();
        const size::CellCountInt last = (size::CellCountInt)(p.node->cols() - 1);
        p.x = end().x < last ? end().x : last;
        return p;
    }
    default: {
        Pin p = end();
        const size::CellCountInt last = (size::CellCountInt)(p.node->cols() - 1);
        p.x = start().x < last ? start().x : last;
        return p;
    }
    }
}

inline Selection::Pin Selection::bottomRight(const Screen *s) const {
    switch (order(s)) {
    case Order::forward: return end();
    case Order::reverse: return start();
    case Order::mirrored_forward: {
        Pin p = end();
        const size::CellCountInt last = (size::CellCountInt)(p.node->cols() - 1);
        p.x = start().x < last ? start().x : last;
        return p;
    }
    default: {
        Pin p = start();
        const size::CellCountInt last = (size::CellCountInt)(p.node->cols() - 1);
        p.x = end().x < last ? end().x : last;
        return p;
    }
    }
}

inline Selection::Order Selection::order(const Screen *s) const {
    const point::Coordinate start_pt = s->pages.pointFromPin(point::Tag::screen, start()).value.c;
    const point::Coordinate end_pt = s->pages.pointFromPin(point::Tag::screen, end()).value.c;

    if (rectangle) {
        /* Reverse (also handles single-column) */
        if (start_pt.y > end_pt.y && start_pt.x >= end_pt.x) return Order::reverse;
        if (start_pt.y >= end_pt.y && start_pt.x > end_pt.x) return Order::reverse;

        /* Mirror, bottom-left to top-right */
        if (start_pt.y > end_pt.y && start_pt.x < end_pt.x) return Order::mirrored_reverse;

        /* Mirror, top-right to bottom-left */
        if (start_pt.y < end_pt.y && start_pt.x > end_pt.x) return Order::mirrored_forward;

        /* Forward */
        return Order::forward;
    }

    if (start_pt.y < end_pt.y) return Order::forward;
    if (start_pt.y > end_pt.y) return Order::reverse;
    if (start_pt.x <= end_pt.x) return Order::forward;
    return Order::reverse;
}

inline Selection Selection::ordered(const Screen *s, Order desired) const {
    if (order(s) == desired) return init(start(), end(), rectangle);

    const Pin tl = topLeft(s);
    const Pin br = bottomRight(s);
    switch (desired) {
    case Order::forward: return init(tl, br, rectangle);
    case Order::reverse: return init(br, tl, rectangle);
    default: return init(tl, br, rectangle);
    }
}

inline bool Selection::contains(const Screen *s, const Pin &pin) const {
    const Pin tl_pin = topLeft(s);
    const Pin br_pin = bottomRight(s);

    /* This is definitely not very efficient. Low-hanging fruit to
     * improve this. */
    const point::Coordinate tl = s->pages.pointFromPin(point::Tag::screen, tl_pin).value.c;
    const point::Coordinate br = s->pages.pointFromPin(point::Tag::screen, br_pin).value.c;
    const point::Coordinate p = s->pages.pointFromPin(point::Tag::screen, pin).value.c;

    /* If we're in rectangle select, we can short-circuit with an easy check
     * here */
    if (rectangle) return p.y >= tl.y && p.y <= br.y && p.x >= tl.x && p.x <= br.x;

    /* If tl/br are same line */
    if (tl.y == br.y) return p.y == tl.y && p.x >= tl.x && p.x <= br.x;

    /* If on top line, just has to be left of X */
    if (p.y == tl.y) return p.x >= tl.x;

    /* If on bottom line, just has to be right of X */
    if (p.y == br.y) return p.x <= br.x;

    /* If between the top/bottom, always good. */
    return p.y > tl.y && p.y < br.y;
}

inline Maybe<Selection> Selection::containedRow(const Screen *s, const Pin &pin) const {
    const Pin tl_pin = topLeft(s);
    const Pin br_pin = bottomRight(s);

    /* This is definitely not very efficient. Low-hanging fruit to
     * improve this. Callers should prefer containedRowCached if they
     * can swing it. */
    const point::Coordinate tl = s->pages.pointFromPin(point::Tag::screen, tl_pin).value.c;
    const point::Coordinate br = s->pages.pointFromPin(point::Tag::screen, br_pin).value.c;
    const point::Coordinate p = s->pages.pointFromPin(point::Tag::screen, pin).value.c;

    return containedRowCached(s, tl_pin, br_pin, pin, tl, br, p);
}

inline void Selection::adjust(const Screen *s, Adjustment adjustment) {
    /* Note that we always adjust "end" because end always represents
     * the last point of the selection by mouse, not necessarily the
     * top/bottom visually. So this results in the correct behavior
     * whether the user drags up or down. */
    Pin *end_pin = endPtr();
    switch (adjustment) {
    case Adjustment::up: {
        const Maybe<Pin> new_end = end_pin->up(1);
        if (new_end.has) {
            *end_pin = new_end.value;
        } else {
            adjust(s, Adjustment::beginning_of_line);
        }
        break;
    }

    case Adjustment::down: {
        /* Find the next non-blank row */
        Pin current = *end_pin;
        bool found = false;
        for (;;) {
            const Maybe<Pin> next = current.down(1);
            if (!next.has) break;
            const page::Page::RowAndCell rac = next.value.rowAndCell();
            const page::Cell *cells = next.value.node->page()->getCells(rac.row);
            if (page::Cell::hasTextAny(cells, next.value.node->page()->size.cols)) {
                *end_pin = next.value;
                found = true;
                break;
            }
            current = next.value;
        }
        /* If we're at the bottom, just go to the end of the line */
        if (!found) adjust(s, Adjustment::end_of_line);
        break;
    }

    case Adjustment::left: {
        PageList::CellIterator it = end_pin->cellIterator(PageList::Direction::left_up, Maybe<Pin>());
        Pin next;
        (void)it.next(&next);
        while (it.next(&next)) {
            if (next.rowAndCell().cell->hasText()) {
                *end_pin = next;
                break;
            }
        }
        break;
    }

    case Adjustment::right: {
        /* Step right, wrapping to the next row down at the start of each new line,
         * until we find a non-empty cell. */
        PageList::CellIterator it = end_pin->cellIterator(PageList::Direction::right_down, Maybe<Pin>());
        Pin next;
        (void)it.next(&next);
        while (it.next(&next)) {
            if (next.rowAndCell().cell->hasText()) {
                *end_pin = next;
                break;
            }
        }
        break;
    }

    case Adjustment::page_up: {
        const Maybe<Pin> new_end = end_pin->up(s->pages.rows);
        if (new_end.has) {
            *end_pin = new_end.value;
        } else {
            adjust(s, Adjustment::home);
        }
        break;
    }

    case Adjustment::page_down: {
        const Maybe<Pin> new_end = end_pin->down(s->pages.rows);
        if (new_end.has) {
            *end_pin = new_end.value;
        } else {
            adjust(s, Adjustment::end);
        }
        break;
    }

    case Adjustment::home: *end_pin = s->pages.pin(point::Point::screen(0, 0)).value; break;

    case Adjustment::end: {
        PageList::RowIterator it = s->pages.rowIterator(PageList::Direction::left_up, point::Point::screen());
        Pin next;
        while (it.next(&next)) {
            const page::Page::RowAndCell rac = next.rowAndCell();
            const size_t len = next.node->page()->size.cols;
            const page::Cell *cells = next.node->page()->getCells(rac.row);
            if (page::Cell::hasTextAny(cells, len)) {
                *end_pin = next;
                end_pin->x = (size::CellCountInt)(len - 1);
                break;
            }
        }
        break;
    }

    case Adjustment::beginning_of_line: end_pin->x = 0; break;

    case Adjustment::end_of_line: end_pin->x = (size::CellCountInt)(end_pin->node->cols() - 1); break;
    }
}

/* ====================================================================== */
/* Screen.zig                                                             */

inline page::PageError Screen::clone(zigstd::Allocator alloc_, const point::Point &top, Maybe<point::Point> bot,
                                     Screen *out) const {
    /* Create a tracked pin remapper for our selection and cursor. Note
     * that we may want to expose this generally in the future but at the
     * time of doing this we don't need to. */
    PageList::Clone::TrackedPinsRemap pin_remap;

    PageList new_pages;
    const page::PageError err = pages.clone(alloc_, PageList::Clone(top, bot, &pin_remap), &new_pages);
    if (err != page::PageError::none) return err;

    /* Find our cursor. If the cursor isn't in the cloned area, we move it
     * to the top-left arbitrarily because a screen must have SOME cursor. */
    Cursor new_cursor;
    {
        bool found = false;
        auto it = pin_remap.find(cursor.page_pin);
        if (it != pin_remap.end()) {
            Pin *p = it->second;
            const Page::RowAndCell page_rac = p->rowAndCell();
            const Maybe<point::Point> pt = new_pages.pointFromPin(point::Tag::active, *p);
            if (pt.has) {
                new_cursor.x = (size::CellCountInt)pt.value.c.x;
                new_cursor.y = (size::CellCountInt)pt.value.c.y;
                new_cursor.page_pin = p;
                new_cursor.page_row = page_rac.row;
                new_cursor.page_cell = page_rac.cell;
                found = true;
            }
        }

        if (!found) {
            Pin *page_pin = new_pages.trackPin(Pin(new_pages.pages.first));
            if (!page_pin) {
                new_pages.deinit();
                return page::PageError::OutOfMemory;
            }
            const Page::RowAndCell page_rac = page_pin->rowAndCell();
            new_cursor.x = 0;
            new_cursor.y = 0;
            new_cursor.page_pin = page_pin;
            new_cursor.page_row = page_rac.row;
            new_cursor.page_cell = page_rac.cell;
        }
    }

    /* Preserve our selection if we have one. */
    Maybe<Selection> sel;
    if (selection.has) {
        const Selection &old = selection.value;
        assert(old.tracked());

        Pin *tl;
        Pin *br;
        switch (old.order(this)) {
        case Selection::Order::forward:
        case Selection::Order::mirrored_forward:
            tl = old.bounds.tracked.start;
            br = old.bounds.tracked.end;
            break;
        default:
            tl = old.bounds.tracked.end;
            br = old.bounds.tracked.start;
            break;
        }

        auto find = [&](Pin *p) -> Pin * {
            auto f = pin_remap.find(p);
            return f == pin_remap.end() ? nullptr : f->second;
        };

        do {
            Pin *start_pin = find(tl);
            if (!start_pin) {
                /* No start means it is outside the cloned area.
                 *
                 * If we have no end pin then either
                 * (1) our whole selection is outside the cloned area or
                 * (2) our cloned area is within the selection */
                if (!find(br)) {
                    /* We check if the selection bottom right pin is above
                     * the cloned area or if the top left pin is below the
                     * cloned area, in either of these cases it means that
                     * the selection is fully out of bounds, so we have no
                     * selection in the cloned area and break out now. */
                    const Maybe<Pin> clone_top = pages.pin(top);
                    if (!clone_top.has) break;
                    const uint32_t clone_top_y = pages.pointFromPin(point::Tag::screen, clone_top.value).value.c.y;
                    if (pages.pointFromPin(point::Tag::screen, *br).value.c.y < clone_top_y) break;
                    if (pages.pointFromPin(point::Tag::screen, *tl).value.c.y > clone_top_y) break;
                }

                /* We move the top pin back in bounds to the top row. */
                PageList::Node *node = new_pages.pages.first;
                const size::CellCountInt last = (size::CellCountInt)(node->cols() - 1);
                start_pin = new_pages.trackPin(Pin(node, 0, old.rectangle ? (tl->x < last ? tl->x : last) : 0));
            }

            /* If we got to this point it means that the selection is not
             * fully out of bounds, so we move the bottom right pin back
             * in bounds if it isn't already. */
            Pin *end_pin = find(br);
            if (!end_pin) {
                PageList::Node *node = new_pages.pages.last;
                const size::CellCountInt last = (size::CellCountInt)(node->cols() - 1);
                end_pin = new_pages.trackPin(Pin(node, (size::CellCountInt)(node->rows() - 1),
                                                 old.rectangle ? (br->x < last ? br->x : last) : last));
            }

            Selection s;
            s.bounds.tag = Selection::Bounds::Tag::tracked;
            s.bounds.tracked.start = start_pin;
            s.bounds.tracked.end = end_pin;
            s.rectangle = old.rectangle;
            sel = s;
        } while (false);
    }

    Screen &result = *out;
    result = Screen();
    result.alloc = alloc_;
    result.pages = new_pages;
    result.no_scrollback = no_scrollback;
    result.cursor = new_cursor;
    result.selection = sel;
    result.dirty = dirty;
    result.assertIntegrity();
    return page::PageError::none;
}

inline PageList::IncreaseCapacityError Screen::increaseCapacity(PageList::Node *node,
                                                                Maybe<PageList::IncreaseCapacity> adjustment,
                                                                PageList::Node **out) {
    /* If the page being modified isn't our cursor page then
     * this is a quick operation because we have no additional
     * accounting. We have to do this check here BEFORE calling
     * increaseCapacity because increaseCapacity will update all
     * our tracked pins (including our cursor). */
    if (node != cursor.page_pin->node) return pages.increaseCapacity(node, adjustment, out);

    /* We're modifying the cursor page. When we increase the
     * capacity below it will be short the ref count on our
     * current style and hyperlink, so we need to init those. */
    PageList::Node *new_node;
    const PageList::IncreaseCapacityError e = pages.increaseCapacity(node, adjustment, &new_node);
    if (e != PageList::IncreaseCapacityError::none) return e;
    Page *new_page = new_node->page();

    /* Re-add the style, if the page somehow doesn't have enough
     * memory to add it, we emit a warning and gracefully degrade
     * to the default style for the cursor. */
    if (cursor.style_id != style::default_id) {
        style::Id id;
        if (new_page->styles.add((const void *)new_page->memory, cursor.style, &id) ==
            ref_counted_set::AddError::none) {
            cursor.style_id = id;
        } else {
            /* TODO: Should we increase the capacity further in this case?
             * log.warn("(Screen.increaseCapacity) Failed to add cursor style back to page")
             *
             * Reset the cursor style. */
            cursor.style = style::Style();
            cursor.style_id = style::default_id;
        }
    }

    /* Re-add the hyperlink, if the page somehow doesn't have enough
     * memory to add it, we emit a warning and gracefully degrade to
     * no hyperlink. */
    if (hyperlink::Hyperlink *link = cursor.hyperlink) {
        /* So we don't attempt to free any memory in the replaced page. */
        cursor.hyperlink_id = 0;
        cursor.hyperlink = nullptr;

        /* Re-add
         * TODO: Should we increase the capacity further in this case?
         * log.warn("(Screen.increaseCapacity) Failed to add cursor hyperlink back to page") */
        (void)startHyperlinkOnce(*link);

        /* Remove our old link */
        link->deinit(alloc);
        alloc.destroy(link);
    }

    /* Reload the cursor information because the pin changed.
     * So our page row/cell and so on are all off. */
    cursorReload();

    *out = new_node;
    return PageList::IncreaseCapacityError::none;
}

inline bool Screen::cursorDownScroll() {
    assert(cursor.y == pages.rows - 1);
    IntegrityGuard g = {this};

    /* If we have no scrollback, then we shift all our rows instead. */
    if (no_scrollback) {
        /* If we have a single-row screen, we have no rows to shift
         * so our cursor is in the correct place we just have to clear
         * the cells. */
        if (pages.rows == 1) {
            Page *page = cursor.page_pin->node->page();
            clearCells(page, cursor.page_row, page->getCells(cursor.page_row), page->size.cols);

            /* The row is a fresh blank row now and must not retain
             * metadata (wrap state, semantic prompt) from the
             * discarded content. */
            cursor.page_row->reset();

            cursorMarkDirty();
        } else {
            /* The call to `eraseRow` will move the tracked cursor pin up by one
             * row, but we don't actually want that, so we keep the old pin and
             * put it back after calling `eraseRow`. */
            const Pin old_pin = *cursor.page_pin;

            /* eraseRow will shift everything below it up. */
            pages.eraseRow(point::Point::active());

            /* Note we don't need to mark anything dirty in this branch
             * because eraseRow will mark all the rotated rows as dirty
             * in the entire page.
             *
             * We don't use `cursorChangePin` here because we aren't
             * actually changing the pin, we're keeping it the same. */
            *cursor.page_pin = old_pin;

            /* We do, however, need to refresh the cached page row
             * and cell, because `eraseRow` will have moved the row. */
            const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
            cursor.page_row = page_rac.row;
            cursor.page_cell = page_rac.cell;
        }
    } else {
        const Pin old_pin = *cursor.page_pin;

        /* Grow our pages by one row. The PageList will handle if we need to
         * allocate, prune scrollback, whatever. */
        PageList::Node *grown;
        if (!pages.grow(&grown)) return false;

        /* Calculate this before cursorChangePin because that function may
         * adjust the underlying page and invalidate references to its pin.
         *
         * If our pin page change it means that the page that the pin
         * was on was pruned. In this case, grow() moves the pin to
         * the top-left of the new page. This effectively moves it by
         * one already, we just need to fix up the x value. */
        Pin new_pin;
        if (old_pin.node == cursor.page_pin->node) {
            new_pin = cursor.page_pin->down(1).value;
        } else {
            new_pin = *cursor.page_pin;
            new_pin.x = cursor.x;
        }

        /* These assertions help catch some pagelist math errors. Our
         * x/y should be unchanged after the grow. */
        if (slow_runtime_safety) {
            const point::Coordinate active = pages.pointFromPin(point::Tag::active, new_pin).value.c;
            assert(active.x == cursor.x);
            assert(active.y == cursor.y);
            (void)active;
        }

        if (cursor.page_pin->node == new_pin.node) {
            /* Scrolling normally stays within one page. Resolve the page once
             * while refreshing the cursor pointers. The cursor already holds
             * live row and cell pointers into this mapping, and grow does not
             * compress pages, so the node must still be resident here. */
            cursorMarkDirty();
            Page *page = new_pin.node->pageAssumeResident();
            const Page::RowAndCell page_rac = page->getRowAndCell(new_pin.x, new_pin.y);
            *cursor.page_pin = new_pin;
            cursor.page_row = page_rac.row;
            cursor.page_cell = page_rac.cell;
        } else {
            /* Crossing a page may require migrating the cursor's style and
             * hyperlink references, so retain the general path here. */
            cursorChangePin(new_pin);
            const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
            cursor.page_row = page_rac.row;
            cursor.page_cell = page_rac.cell;
        }

        /* Our new row is always dirty */
        cursorMarkDirty();

        /* Clear the new row so it gets our bg color. We only do this
         * if we have a bg color at all. */
        if (cursor.style.bg_color.tag != style::Style::Color::Tag::none) {
            Page *page = cursor.page_pin->node->page();
            clearCells(page, cursor.page_row, page->getCells(cursor.page_row), page->size.cols);
        }
    }

    if (cursor.style_id != style::default_id) {
        /* The newly created line needs to be styled according to
         * the bg color if it is set. */
        Cell blank_cell;
        if (cursor.style.bgCell(&blank_cell)) {
            Cell *cells = cursor.page_cell - cursor.x;
            for (size_t i = 0; i < pages.cols; i++) cells[i] = blank_cell;
        }
    }
    return true;
}

inline bool Screen::cursorScrollAbove() {
    /* We unconditionally mark the cursor row as dirty here because
     * the cursor always changes page rows inside this function, and
     * when that happens it can mean the text in the old row needs to
     * be re-shaped because the cursor splits runs to break ligatures. */
    cursorMarkDirty();

    /* If the cursor is on the bottom of the screen, its faster to use
     * our specialized function for that case. */
    if (cursor.y == pages.rows - 1) {
        return cursorDownScroll();
    }

    IntegrityGuard g = {this};

    /* Logic below assumes we always have at least one row that isn't moving */
    assert(cursor.y < pages.rows - 1);

    /* Explanation:
     *  We don't actually move everything that's at or above the cursor row,
     *  since this would require us to shift up our ENTIRE scrollback, which
     *  would be ridiculously expensive. Instead, we insert a new row at the
     *  end of the pagelist (`grow()`), and move everything BELOW the cursor
     *  DOWN by one row. This has the same practical result but it's a whole
     *  lot cheaper in 99% of cases. */

    const Pin old_pin = *cursor.page_pin;
    PageList::Node *new_node;
    if (!pages.grow(&new_node)) return false;
    if (new_node) {
        if (!cursorScrollAboveRotate(new_node)) return false;
    } else {
        /* In this case, it means grow() didn't allocate a new page. */

        if (cursor.page_pin->node == pages.pages.last) {
            /* If we're on the last page we can do a very fast path because
             * all the rows we need to move around are within a single page.
             *
             * Note: we don't need to call cursorChangePin here because
             * the pin page is the same so there is no accounting to do
             * for styles or any of that. */
            assert(old_pin.node == cursor.page_pin->node);
            (void)old_pin;
            *cursor.page_pin = cursor.page_pin->down(1).value;

            Pin *pin = cursor.page_pin;
            Page *page = cursor.page_pin->node->page();

            /* Rotate the rows so that the newly created empty row is at the
             * beginning. e.g. [ 0 1 2 3 ] in to [ 3 0 1 2 ]. */
            Row *rows = page->rows.ptr(page->memory);
            /* Rotating this suffix changes which logical row its coordinates identify. */
            pages.invalidateNodeLayout(pin->node);
            fastmem::rotateOnceR<Row>(rows + pin->y, (size_t)page->size.rows - pin->y);

            /* Mark the whole page as dirty.
             *
             * Technically we only need to mark from the cursor row to the
             * end but this is a hot function, so we want to minimize work. */
            page->dirty = true;

            /* Setup our cursor caches after the rotation so it points to the
             * correct data */
            const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
            cursor.page_row = page_rac.row;
            cursor.page_cell = page_rac.cell;
        } else {
            /* We didn't grow pages but our cursor isn't on the last page.
             * In this case we need to do more work because we need to copy
             * elements between pages. */
            if (!cursorScrollAboveRotate(nullptr)) return false;
        }
    }

    if (cursor.style_id != style::default_id) {
        /* The newly created line needs to be styled according to
         * the bg color if it is set. */
        Cell blank_cell;
        if (cursor.style.bgCell(&blank_cell)) {
            Cell *cells = cursor.page_cell - cursor.x;
            for (size_t i = 0; i < pages.cols; i++) cells[i] = blank_cell;
        }
    }
    return true;
}

inline bool Screen::cursorScrollAboveRotate(PageList::Node *fresh_node) {
    /* A fresh node given to us is always the newly allocated tail of
     * the page list (see grow), which is where our iteration starts. */
    assert(fresh_node == nullptr || fresh_node == pages.pages.last);

    cursorChangePin(cursor.page_pin->down(1).value);

    /* Go through each of the pages following our pin, shift all rows
     * down by one, and copy the last row of the previous page. We start
     * at the tail, which is the only node that can be freshly allocated. */
    PageList::Node *current = pages.pages.last;
    bool current_is_fresh = fresh_node != nullptr;
    while (current != cursor.page_pin->node) {
        PageList::Node *prev = current->prev;

        /* A newly allocated tail has no earlier references to invalidate. */
        if (!current_is_fresh) {
            /* Rotating this page moves every cached row coordinate down by one. */
            pages.invalidateNodeLayout(current);
        }

        /* Rotate the pages down: [ 0 1 2 3 ] => [ 3 0 1 2 ] */
        {
            Page *cur_page = current->page();
            Row *cur_rows = cur_page->rows.ptr(cur_page->memory);
            fastmem::rotateOnceR<Row>(cur_rows, cur_page->size.rows);
        }

        /* Copy the last row of the previous page to the top of current.
         * If the current page doesn't have enough capacity for the
         * managed memory of the copied row (styles, hyperlinks, etc.)
         * then its capacity is increased and the copy retried, which
         * may replace `current` in the page list. Our loop condition
         * guarantees `current` is never the cursor page, and `prev` is
         * unaffected by the replacement. */
        {
            Page *prev_page = prev->page();
            Row *prev_rows = prev_page->rows.ptr(prev_page->memory);
            current = clonePartialRowGrowCapacity(current, 0, prev_page, &prev_rows[prev_page->size.rows - 1], 0,
                                                  pages.cols);
        }

        /* Mark dirty on the page, since we are dirtying all rows with this. */
        current->page()->dirty = true;

        current = current->prev;
        current_is_fresh = false;
    }

    /* Our current is our cursor page, we need to rotate down from
     * our cursor and clear our row. */
    assert(current == cursor.page_pin->node);
    Page *cur_page = current->page();
    Row *cur_rows = cur_page->rows.ptr(cur_page->memory);
    /* Rotating the cursor-page suffix changes its cached row coordinates. */
    pages.invalidateNodeLayout(current);
    fastmem::rotateOnceR<Row>(cur_rows + cursor.page_pin->y, (size_t)cur_page->size.rows - cursor.page_pin->y);
    clearCells(cur_page, &cur_rows[cursor.page_pin->y], cur_page->getCells(&cur_rows[cursor.page_pin->y]),
               cur_page->size.cols);

    /* The recycled storage becomes the new blank cursor row and must
     * not retain metadata (wrap state, semantic prompt) from the row
     * whose content was moved to the next page. */
    cur_rows[cursor.page_pin->y].reset();

    /* Mark the whole page as dirty.
     *
     * Technically we only need to mark from the cursor row to the
     * end but this is a hot function, so we want to minimize work. */
    cur_page->dirty = true;

    /* Setup cursor cache data after all the rotations so our
     * row is valid. */
    const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
    cursor.page_row = page_rac.row;
    cursor.page_cell = page_rac.cell;
    return true;
}

inline bool Screen::cursorScrollRegionUp(size_t limit) {
    assert(limit >= 1);
    assert(cursor.y >= limit);
    IntegrityGuard g = {this};

    Pin *pin = cursor.page_pin;

    /* If the region crosses a page boundary we take a slower path. This
     * is rare: it requires the active area to span multiple pages with
     * the split point inside the scroll region. */
    if (pin->y < limit) return cursorScrollRegionUpSlow(limit);

    /* Fast path: the entire region is in a single page. We can clear
     * the top row and rotate it down to the cursor row, updating any
     * tracked pins along the way.
     *
     * The cursor's cached row and cell pointers refer into this mapping.
     * PageList cannot compress the cursor page while those pointers are
     * installed, so the node is known to be resident here. */
    Page *page = pin->node->pageAssumeResident();
    Row *rows = page->rows.ptr(page->memory) + (pin->y - limit);

    /* Clear the erased (top) row. */
    {
        Row *row = &rows[0];

        /* Whether our blank cell is a plain zero cell. This is true
         * unless the cursor has a background color set (see blankCell). */
        const bool blank_is_zero = cursor.style_id == style::default_id ||
                                   cursor.style.bg_color.tag == style::Style::Color::Tag::none;

        if (!row->managedMemory() && blank_is_zero) {
            /* Hot path: the row has no managed memory (styles,
             * graphemes, hyperlinks) and our blank is zero so this is
             * a straight zero fill. This is the overwhelmingly common
             * case for scroll region usage. */
            Cell *cells = page->getCells(row);
            memset(cells, 0, sizeof(Cell) * page->size.cols);
        } else {
            /* The generic clear handles managed memory and fills the
             * row with our blank cell, preserving the background color. */
            clearCells(page, row, page->getCells(row), page->size.cols);
        }

        /* The row becomes the new blank cursor row after the rotation
         * below and must not retain metadata (wrap state, semantic
         * prompt) from the discarded content. */
        row->reset();
    }

    /* Rotate the region rows so the now-blank top row moves to the
     * bottom (the cursor row) and everything else shifts up by one.
     * Rotating the region changes which logical row its coordinates identify. */
    pages.invalidateNodeLayout(pin->node);
    fastmem::rotateOnce<Row>(rows, limit + 1);

    /* Mark the whole page as dirty.
     *
     * Technically we only need to mark the rotated rows but this is
     * a hot function, so we want to minimize work. */
    page->dirty = true;

    /* If our viewport is a pin and it's within the rotated region
     * then we need to shift its cached row offset up. See
     * PageList.eraseRowBounded for details; this mirrors that logic. */
    if (pages.viewport == PageList::Viewport::pin && pages.viewport_pin_row_offset.has) {
        const Pin *p = pages.viewport_pin;
        if (!(p->node != pin->node || p->y < pin->y - limit || p->y > pin->y || p->y == 0))
            pages.viewport_pin_row_offset.value -= 1;
    }

    /* Update tracked pins within the region since their rows moved up
     * by one. The cursor's own pin is skipped because the cursor stays
     * at the region bottom (the new blank row). */
    for (size_t i = 0; i < pages.tracked_pins.count(); i++) {
        Pin *p = pages.tracked_pins.keys()[i];
        if (p->node != pin->node || p == pin || p->y < pin->y - limit || p->y > pin->y) continue;
        if (p->y == 0)
            p->x = 0;
        else
            p->y -= 1;
    }

    /* The cursor pin is unchanged, but the Row structure at the pin
     * position now contains the blank row, so we need to refresh our
     * cached row/cell pointers. We compute them directly from the row
     * slice we already have rather than going through the pin since
     * this is a hot path. */
    Row *cursor_row = &rows[limit];
    cursor.page_row = cursor_row;
    cursor.page_cell = &page->getCells(cursor_row)[cursor.x];
    return true;
}

inline bool Screen::cursorScrollRegionUpSlow(size_t limit) {
    /* The call to eraseRowBounded below will move our tracked cursor
     * pin up by one row since it is inside the erased region, but we
     * don't actually want that: the cursor stays put, on the new blank
     * row at the region bottom. We keep the old pin and put it back
     * after, exactly like cursorDownScroll does for the no-scrollback
     * case.
     *
     * This matters beyond performance: when the cursor is on the first
     * row of a page, eraseRowBounded moves the tracked pin to the
     * previous page. Moving the cursor back down with cursorDown would
     * then cross pages via cursorChangePin, which migrates the cursor
     * style refcount from a page that never held it, corrupting the
     * style accounting. */
    const Pin old_pin = *cursor.page_pin;

    pages.eraseRowBounded(point::Point::active(0, (uint32_t)(cursor.y - limit)), limit);

    /* We don't use `cursorChangePin` here because we aren't actually
     * changing the pin, we're keeping it the same. Since the page
     * never changes, the cursor's style ref stays valid and no style
     * accounting needs to be updated. */
    *cursor.page_pin = old_pin;

    /* We do, however, need to refresh the cached page row and cell,
     * because the row contents at our pin position changed (it now
     * contains the blank row). */
    const Page::RowAndCell page_rac = cursor.page_pin->rowAndCell();
    cursor.page_row = page_rac.row;
    cursor.page_cell = page_rac.cell;

    /* eraseRowBounded clears the new row with zero cells so if our
     * blank cell isn't zero (bg color is set) we need to fill it. */
    const Cell blank = blankCell();
    if (!blank.isZero()) {
        Cell *cells = cursor.page_cell - cursor.x;
        for (size_t i = 0; i < pages.cols; i++) cells[i] = blank;
    }
    return true;
}

inline PageList::IncreaseCapacityError Screen::cursorCopy(const Cursor &other, bool copy_hyperlink) {
    assert(other.x < pages.cols);
    assert(other.y < pages.rows);

    /* End any currently active hyperlink on our cursor. */
    endHyperlink();

    const Cursor old = cursor;
    cursor = other;

    /* Keep our old style ID so it can be properly cleaned up below. */
    cursor.style_id = old.style_id;

    /* Hyperlinks will be managed separately below. */
    cursor.hyperlink_id = 0;
    cursor.hyperlink = nullptr;

    /* Keep our old page pin and X/Y because:
     * 1. The old style will need to be cleaned up from the page it's from.
     * 2. The new position navigated to by `cursorAbsolute` needs to be in our
     *    own screen. */
    cursor.page_pin = old.page_pin;
    cursor.x = old.x;
    cursor.y = old.y;

    /* Call manual style update in order to clean up our old style, if we have
     * one, and also to load the style from the other cursor, if it had one. */
    const PageList::IncreaseCapacityError e = manualStyleUpdate();
    if (e != PageList::IncreaseCapacityError::none) {
        cursor = old;
        return e;
    }

    /* Move to the correct location to match the other cursor. */
    cursorAbsolute(other.x, other.y);

    /* If the other cursor had a hyperlink, add it to ours. */
    if (copy_hyperlink && other.hyperlink_id != 0) {
        /* Get the hyperlink from the other cursor's page. */
        const Page *other_page = other.page_pin->node->page();
        const hyperlink::PageEntry *other_link =
            other_page->hyperlink_set.get((const void *)other_page->memory, other.hyperlink_id);

        const uint8_t *uri = other_link->uri.slice((const void *)other_page->memory);
        const uint8_t *id = nullptr;
        size_t id_len = 0;
        if (other_link->id.tag == hyperlink::PageEntry::Id::Tag::explicit_) {
            id = other_link->id.explicit_.slice((const void *)other_page->memory);
            id_len = other_link->id.explicit_.len;
        }

        /* And it to our cursor. This shouldn't fail because startHyperlink
         * should handle resizing. This only happens if we're truly out of
         * RAM. Degrade to forgetting the hyperlink.
         * log.err("failed to update hyperlink on cursor change err={}") */
        (void)startHyperlink(uri, other_link->uri.len, id, id_len);
    }
    return PageList::IncreaseCapacityError::none;
}

inline void Screen::cursorChangePin(const Pin &new_) {
    /* Moving the cursor affects text run splitting (ligatures) so
     * we must mark the old and new page dirty. We do this as long
     * as the pins are not equal */
    if (!cursor.page_pin->eql(new_)) {
        cursorMarkDirty();
        new_.markDirty();
    }

    /* If our pin is on the same page, then we can just update the pin.
     * We don't need to migrate any state. */
    if (cursor.page_pin->node == new_.node) {
        *cursor.page_pin = new_;
        return;
    }

    /* If we have an old style then we need to release it from the old page. */
    Maybe<style::Style> old_style_;
    if (cursor.style_id != style::default_id) old_style_ = cursor.style;
    if (old_style_.has) {
        /* Release the style directly from the old page instead of going through
         * manualStyleUpdate, because the cursor position may have already been
         * updated but the pin has not, which would fail integrity checks. */
        Page *old_page = cursor.page_pin->node->page();
        old_page->styles.release((const void *)old_page->memory, cursor.style_id);
        cursor.style = style::Style();
        cursor.style_id = style::default_id;
    }

    /* If we have a hyperlink then we need to release it from the old page. */
    if (cursor.hyperlink != nullptr) {
        Page *old_page = cursor.page_pin->node->page();
        old_page->hyperlink_set.release((const void *)old_page->memory, cursor.hyperlink_id);
        /* Zero the ID, it is invalid now and style changes below may
         * run integrity checks. We still have self.cursor.hyperlink to
         * rebuild this later. */
        cursor.hyperlink_id = 0;
    }

    /* Update our pin to the new page */
    *cursor.page_pin = new_;

    /* On the new page, we need to migrate our style */
    if (old_style_.has) {
        cursor.style = old_style_.value;
        if (manualStyleUpdate() != PageList::IncreaseCapacityError::none) {
            /* This failure should not happen because manualStyleUpdate
             * handles page splitting, overflow, and more. This should only
             * happen if we're out of RAM. In this case, we'll just degrade
             * gracefully back to the default style.
             * log.err("failed to update style on cursor change err={}") */
            cursor.style = style::Style();
            cursor.style_id = 0;
        }
    }

    /* On the new page, we need to migrate our hyperlink */
    if (hyperlink::Hyperlink *link = cursor.hyperlink) {
        /* startHyperlink will try to free old hyperlinks, so set this
         * to null. We free it ourselves later since we're doing some
         * ref-counting shenanigans in this function. */
        cursor.hyperlink = nullptr;

        /* Re-add. This shouldn't fail because startHyperlink should handle
         * resizing. This only happens if we're truly out of RAM. Degrade
         * to forgetting the hyperlink.
         * log.err("failed to update hyperlink on cursor change err={}") */
        const bool explicit_ = link->id.tag == hyperlink::Hyperlink::Id::Tag::explicit_;
        (void)startHyperlink(link->uri, link->uri_len, explicit_ ? link->id.explicit_ptr : nullptr,
                             explicit_ ? link->id.explicit_len : 0);

        /* Remove our old link */
        link->deinit(alloc);
        alloc.destroy(link);
    }
}

inline bool Screen::resize(const Resize &opts) {
    typedef resize_tw tw;
    IntegrityGuard g = {this};

    /* We need to insert a tracked pin for our saved cursor so we can
     * modify its X/Y for reflow. Do this before changing any state since
     * tracking the pin can fail. */
    Pin *saved_cursor_pin = nullptr;
    if (saved_cursor.has) {
        const SavedCursor &sc = saved_cursor.value;
        const Maybe<Pin> p = pages.pin(point::Point::active(sc.x, sc.y));
        if (p.has) {
            if (tw::check(ResizeTw::saved_cursor_pin) != AllocTw::none) return false;
            saved_cursor_pin = pages.trackPin(p.value);
            if (!saved_cursor_pin) return false;
        }
    }
    struct UntrackGuard {
        PageList *pl;
        Pin *p;
        ~UntrackGuard() {
            if (p) pl->untrackPin(p);
        }
    } untrack = {&pages, saved_cursor_pin};

    /* A cursor style and hyperlink are partly stored in the page containing
     * the cursor. Their IDs only have meaning within that page, and the page
     * keeps reference counts for them. Resizing can replace or destroy the
     * page, so below we temporarily remove both values from the cursor before
     * asking PageList to resize.
     *
     * Save everything first so we can put the cursor back together afterward,
     * or restore it unchanged if the resize fails. We also save the original
     * node and serial so after a successful resize we can tell whether that
     * page survived and still needs its temporary references released. */
    PageList::Node *cursor_node = cursor.page_pin->node;
    const uint64_t cursor_node_serial = cursor_node->serial;
    const style::Style cursor_style = cursor.style;
    const style::Id cursor_style_id = cursor.style_id;
    hyperlink::Hyperlink *cursor_hyperlink = cursor.hyperlink;
    const hyperlink::Id cursor_hyperlink_id = cursor.hyperlink_id;

    /* Keep an extra reference to the cursor style and hyperlink while the
     * resize is in progress. Releasing the cursor references below therefore
     * can't delete either entry, and an error can restore the cursor without
     * any allocation. */
    {
        Page *page = cursor_node->page();
        if (cursor_style_id != style::default_id) {
            page->styles.use((const void *)page->memory, cursor_style_id);
        }
        if (cursor_hyperlink_id != 0) {
            page->hyperlink_set.use((const void *)page->memory, cursor_hyperlink_id);
        }
    }
    auto errdefer = [&]() {
        cursor.style = cursor_style;
        cursor.style_id = cursor_style_id;
        cursor.hyperlink = cursor_hyperlink;
        cursor.hyperlink_id = cursor_hyperlink_id;
    };

    /* Release the cursor style while resizing just in case the cursor ends
     * up on a different page. */
    cursor.style = style::Style();
    (void)manualStyleUpdate();

    /* If we have a hyperlink, release it from the old page
     * and then we need to re-add it to the new page. This needs
     * to happen because resize below typically reallocates a
     * new page so the old hyperlink is invalid. */
    if (cursor.hyperlink_id != 0) {
        /* Note we do NOT use endHyperlink because we want to keep
         * our allocated self.cursor.hyperlink valid. */
        Page *page = cursor.page_pin->node->page();
        page->hyperlink_set.release((const void *)page->memory, cursor.hyperlink_id);
        cursor.hyperlink_id = 0;
        cursor.hyperlink = nullptr;
    }

    /* Perform the resize operation. */
    if (tw::check(ResizeTw::pages) != AllocTw::none) {
        errdefer();
        return false;
    }
    {
        PageList::Resize r;
        r.rows = opts.rows;
        r.cols = opts.cols;
        r.reflow = opts.reflow;
        r.cursor = PageList::Resize::Cursor(cursor.x, cursor.y, cursor.page_pin);
        r.pull_scrollback = opts.pull_scrollback;
        if (!pages.resize(r)) {
            errdefer();
            return false;
        }
    }

    /* No more failures are possible after this. Enforced by compiler
     * because we do NO cleanup of the state below.
     *
     * If we have no scrollback and we shrunk our rows, we must explicitly
     * erase our history. This is because PageList always keeps at least
     * a page size of history. */
    if (no_scrollback) pages.eraseHistory(Maybe<point::Point>());

    /* If our cursor was updated, we do a full reload so all our cursor
     * state is correct. */
    cursorReload();

    /* Clear any redrawable prompt after the fallible resize but before
     * restoring the cursor style and hyperlink, so cleared cells retain the
     * same default styling they had with the previous ordering. */
    clearPromptForRedraw(opts.prompt_redraw);

    /* Restore the cursor style. */
    cursor.style = cursor_style;
    if (manualStyleUpdate() != PageList::IncreaseCapacityError::none) {
        /* This failure should not happen because manualStyleUpdate handles
         * page splitting, overflow, and more. This should only happen if
         * we're out of RAM. In this case, we'll just degrade gracefully back
         * to the default style.
         * log.err("failed to update style on cursor reload err={}") */
        cursor.style = style::Style();
        cursor.style_id = 0;
    }

    /* If we reflowed a saved cursor, update it. */
    if (saved_cursor_pin) {
        /* This should never fail because a non-null saved_cursor_pin
         * implies a non-null saved_cursor. */
        SavedCursor &sc = saved_cursor.value;
        const Maybe<point::Point> pt = pages.pointFromPin(point::Tag::active, *saved_cursor_pin);
        if (pt.has) {
            sc.x = (size::CellCountInt)pt.value.c.x;
            sc.y = (size::CellCountInt)pt.value.c.y;

            /* If we had pending wrap set and we're no longer at the end of
             * the line, we unset the pending wrap and move the cursor to
             * reflect the correct next position. */
            if (sc.pending_wrap && sc.x != opts.cols - 1) {
                sc.pending_wrap = false;
                sc.x += 1;
            }
        } else {
            /* I think this can happen if the screen is resized to be
             * less rows or less cols and our saved cursor moves outside
             * the active area. In this case, there isn't anything really
             * reasonable we can do so we just move the cursor to the
             * top-left. It may be reasonable to also move the cursor to
             * match the primary cursor. Any behavior is fine since this is
             * totally unspecified. */
            sc.x = 0;
            sc.y = 0;
            sc.pending_wrap = false;
        }
    }

    /* Fix up our hyperlink if we had one. */
    if (cursor_hyperlink) {
        /* This shouldn't happen because startHyperlink should handle
         * resizing. This only happens if we're truly out of RAM. Degrade
         * to forgetting the hyperlink.
         * log.err("failed to update hyperlink on resize err={}") */
        const bool explicit_ = cursor_hyperlink->id.tag == hyperlink::Hyperlink::Id::Tag::explicit_;
        (void)startHyperlink(cursor_hyperlink->uri, cursor_hyperlink->uri_len,
                             explicit_ ? cursor_hyperlink->id.explicit_ptr : nullptr,
                             explicit_ ? cursor_hyperlink->id.explicit_len : 0);

        /* Remove our old link */
        cursor_hyperlink->deinit(alloc);
        alloc.destroy(cursor_hyperlink);
    }

    /* A tracked pin follows its content when PageList moves or replaces a
     * page. If the cursor's pin still points to the node we started with,
     * that node survived the resize and still owns the temporary style and
     * hyperlink references we added above.
     *
     * Comparing pointers is not enough by itself. PageList keeps a pool of
     * nodes, so it can destroy a node and create a replacement at the same
     * memory address. Every new use of a node gets a different serial number,
     * making the pointer-and-serial pair an O(1) identity check. */
    const bool cursor_node_survived =
        cursor.page_pin->node == cursor_node && cursor.page_pin->node->serial == cursor_node_serial;
    if (slow_runtime_safety) {
        assert(cursor_node_survived == pages.nodeIsValid(cursor_node, cursor_node_serial));
    }

    /* A surviving node still contains our temporary references, so remove
     * them now. If the node was replaced, its destruction removed those
     * references along with the rest of the old page so we don't need to
     * fix it up. */
    if (cursor_node_survived) {
        Page *page = cursor_node->page();
        if (cursor_style_id != style::default_id) {
            page->styles.release((const void *)page->memory, cursor_style_id);
        }
        if (cursor_hyperlink_id != 0) {
            page->hyperlink_set.release((const void *)page->memory, cursor_hyperlink_id);
        }
    }
    return true;
}

inline void Screen::clearPromptForRedraw(terminal::osc::semantic_prompt::Redraw redraw) {
    typedef terminal::osc::semantic_prompt::Redraw Redraw;
    /* If our cursor is on a prompt or input line, clear it so the shell can
     * redraw it. This works with OSC 133 semantic prompts. We do this after
     * the fallible resize so an error leaves the original prompt untouched.
     *
     * We check cursor.semantic_content rather than page_row.semantic_prompt
     * because some shells (e.g., Nu) mark input areas with OSC 133 B but don't
     * mark continuation lines with k=s. If the input spans multiple lines and
     * continuation lines are unmarked, checking only page_row.semantic_prompt
     * would miss them. By checking semantic_content, we assume that if the
     * cursor is on anything other than command output, we're at a prompt/input
     * line and should clear from there. */
    if (redraw == Redraw::false_ || cursor.semantic_content == Cell::SemanticContent::output) return;

    switch (redraw) {
    /* For `.last`, only clear the current line where the cursor is.
     * For `.true`, clear all prompt lines starting from the beginning. */
    case Redraw::last: {
        Page *page = cursor.page_pin->node->page();
        Row *row = cursor.page_row;
        clearCells(page, row, page->getCells(row), page->size.cols);
        break;
    }

    case Redraw::true_: {
        PageList::PromptIterator pit = cursor.page_pin->promptIterator(PageList::Direction::left_up, Maybe<Pin>());
        Pin start;
        if (!pit.next(&start)) {
            /* This should never happen because promptIterator should always
             * find a prompt if we already verified our row is some kind of
             * prompt.
             * log.warn("cursor on prompt line but promptIterator found no prompt") */
            return;
        }

        /* Clear cells from our start down. We replace it with spaces,
         * and do not physically erase the rows (eraseRows) because the
         * shell is going to expect this space to be available. */
        PageList::RowIterator it = start.rowIterator(PageList::Direction::right_down, Maybe<Pin>());
        Pin p;
        while (it.next(&p)) {
            Page *page = p.node->page();
            Row *row = p.rowAndCell().row;
            clearCells(page, row, page->getCells(row), page->size.cols);
        }
        break;
    }

    default: break;
    }
}

inline PageList::IncreaseCapacityError Screen::setAttribute(const terminal::sgr::Attribute &attr) {
    typedef terminal::sgr::Attribute::Tag T;
    typedef style::Style::Color C;
    /* If we fail to set our style for any reason, we should revert
     * back to the old style. If we fail to do that, we revert back to
     * the default style. */
    const style::Style old_style = cursor.style;

    switch (attr.tag) {
    case T::unset: cursor.style = style::Style(); break;
    case T::bold: cursor.style.flags.bold = true; break;
    case T::reset_bold:
        /* Bold and faint share the same SGR code for this */
        cursor.style.flags.bold = false;
        cursor.style.flags.faint = false;
        break;
    case T::italic: cursor.style.flags.italic = true; break;
    case T::reset_italic: cursor.style.flags.italic = false; break;
    case T::faint: cursor.style.flags.faint = true; break;
    case T::underline: cursor.style.flags.underline = attr.underline; break;
    case T::underline_color:
        cursor.style.underline_color = C::makeRgb(style::RGB(attr.rgb.r, attr.rgb.g, attr.rgb.b));
        break;
    case T::underline_color_256: cursor.style.underline_color = C::makePalette(attr.index); break;
    case T::reset_underline_color: cursor.style.underline_color = C::none(); break;
    case T::overline: cursor.style.flags.overline = true; break;
    case T::reset_overline: cursor.style.flags.overline = false; break;
    case T::blink: cursor.style.flags.blink = true; break;
    case T::reset_blink: cursor.style.flags.blink = false; break;
    case T::inverse: cursor.style.flags.inverse = true; break;
    case T::reset_inverse: cursor.style.flags.inverse = false; break;
    case T::invisible: cursor.style.flags.invisible = true; break;
    case T::reset_invisible: cursor.style.flags.invisible = false; break;
    case T::strikethrough: cursor.style.flags.strikethrough = true; break;
    case T::reset_strikethrough: cursor.style.flags.strikethrough = false; break;
    case T::direct_color_fg:
        cursor.style.fg_color = C::makeRgb(style::RGB(attr.rgb.r, attr.rgb.g, attr.rgb.b));
        break;
    case T::direct_color_bg:
        cursor.style.bg_color = C::makeRgb(style::RGB(attr.rgb.r, attr.rgb.g, attr.rgb.b));
        break;
    case T::fg_8: cursor.style.fg_color = C::makePalette((uint8_t)attr.name); break;
    case T::bg_8: cursor.style.bg_color = C::makePalette((uint8_t)attr.name); break;
    case T::reset_fg: cursor.style.fg_color = C::none(); break;
    case T::reset_bg: cursor.style.bg_color = C::none(); break;
    case T::bright_fg_8: cursor.style.fg_color = C::makePalette((uint8_t)attr.name); break;
    case T::bright_bg_8: cursor.style.bg_color = C::makePalette((uint8_t)attr.name); break;
    case T::fg_256: cursor.style.fg_color = C::makePalette(attr.index); break;
    case T::bg_256: cursor.style.bg_color = C::makePalette(attr.index); break;
    case T::unknown: return PageList::IncreaseCapacityError::none;
    }

    /* If the attribute didn't change our style then we can skip the
     * style update entirely: our current style ID is already correct.
     * This is a common case in the wild where programs re-assert the
     * same style repeatedly (e.g. per span or per line). */
    if (cursor.style.eql(old_style)) return PageList::IncreaseCapacityError::none;

    const PageList::IncreaseCapacityError e = manualStyleUpdate();
    if (e != PageList::IncreaseCapacityError::none) {
        cursor.style = old_style;
        if (manualStyleUpdate() != PageList::IncreaseCapacityError::none) {
            /* log.warn("setAttribute error restoring old style after failure err={}") */
            cursor.style = style::Style();
            (void)manualStyleUpdate();
        }
    }
    return e;
}

inline PageList::IncreaseCapacityError Screen::manualStyleUpdate() {
    IntegrityGuard g = {this};
    Page *page = cursor.page_pin->node->page();

    /* Release our previous style if it was not default. */
    if (cursor.style_id != style::default_id) {
        page->styles.release((const void *)page->memory, cursor.style_id);
    }

    /* If our new style is the default, just reset to that */
    if (cursor.style.default_()) {
        cursor.style_id = style::default_id;
        return PageList::IncreaseCapacityError::none;
    }

    /* Clear the cursor style ID to prevent weird things from happening
     * if the page capacity has to be adjusted which would end up calling
     * manualStyleUpdate again.
     *
     * This also ensures that if anything fails below, we fall back to
     * clearing our style. */
    cursor.style_id = style::default_id;

    /* After setting the style, we need to update our style map.
     * Note that we COULD lazily do this in print. We should look into
     * if that makes a meaningful difference. Our priority is to keep print
     * fast because setting a ton of styles that do nothing is uncommon
     * and weird. */
    style::Id id;
    const ref_counted_set::AddError err = page->styles.add((const void *)page->memory, cursor.style, &id);
    if (err != ref_counted_set::AddError::none) {
        /* Our style map is full or needs to be rehashed, so we need to
         * increase style capacity (or rehash). */
        PageList::Node *node;
        const PageList::IncreaseCapacityError ie = increaseCapacity(
            cursor.page_pin->node,
            err == ref_counted_set::AddError::OutOfMemory ? Maybe<PageList::IncreaseCapacity>(PageList::IncreaseCapacity::styles)
                                                          : Maybe<PageList::IncreaseCapacity>(),
            &node);
        switch (ie) {
        case PageList::IncreaseCapacityError::none: break;
        case PageList::IncreaseCapacityError::OutOfMemory: return PageList::IncreaseCapacityError::OutOfMemory;
        case PageList::IncreaseCapacityError::OutOfSpace: {
            /* Out of space, we need to split the page. Split wherever
             * is using less capacity and hope that works. If it doesn't
             * work, we tried. */
            const PageList::SplitError se = splitForCapacity(*cursor.page_pin);
            if (se == PageList::SplitError::OutOfMemory) return PageList::IncreaseCapacityError::OutOfMemory;
            if (se == PageList::SplitError::OutOfSpace) return PageList::IncreaseCapacityError::OutOfSpace;
            node = cursor.page_pin->node;
            break;
        }
        }

        page = node->page();
        const ref_counted_set::AddError err2 = page->styles.add((const void *)page->memory, cursor.style, &id);
        switch (err2) {
        case ref_counted_set::AddError::none: break;
        case ref_counted_set::AddError::OutOfMemory:
            /* This shouldn't happen because increaseCapacity is
             * guaranteed to increase our capacity by at least one and
             * we only need one space, but again, I don't want to crash
             * here so let's log loudly and reset.
             * log.err("style addition failed after capacity increase") */
            return PageList::IncreaseCapacityError::OutOfMemory;
        case ref_counted_set::AddError::NeedsRehash:
            /* This should be impossible because we rehash above
             * and rehashing should never result in a duplicate. But
             * we don't want to simply hard crash so log it and
             * clear our style.
             * log.err("style rehash resulted in needs rehash") */
            return PageList::IncreaseCapacityError::none;
        }
    }

    cursor.style_id = id;
    return PageList::IncreaseCapacityError::none;
}

inline PageList::SplitError Screen::splitForCapacity(const Pin &pin) {
    /* Get our capacities. We include our target row because its
     * capacity will be preserved. */
    const size_t bytes_above = Page::layout(pin.node->page()->exactRowCapacity(0, (size_t)pin.y + 1)).total_size;
    const size_t bytes_below = Page::layout(pin.node->page()->exactRowCapacity(pin.y, pin.node->rows())).total_size;

    /* We need to track the old cursor pin because if our split
     * moves the cursor pin we need to update our accounting. */
    const Pin old_cursor = *cursor.page_pin;

    /* If our bytes above are less than bytes below, we move the pin
     * to split down one since splitting includes the pinned row in
     * the new node. */
    Pin target = pin;
    if (bytes_above < bytes_below) {
        const Maybe<Pin> d = pin.down(1);
        if (d.has) target = d.value;
    }
    const PageList::SplitError e = pages.split(target);
    if (e != PageList::SplitError::none) return e;

    /* Cursor didn't change nodes, we're done. */
    if (cursor.page_pin->node == old_cursor.node) return PageList::SplitError::none;

    /* Cursor changed, we need to restore the old pin then use
     * cursorChangePin to move to the new pin. The old node is guaranteed
     * to still exist, just not the row.
     *
     * Note that page_row and all that will be invalid, it points to the
     * new node, but at the time of writing this we don't need any of that
     * to be right in cursorChangePin. */
    const Pin new_cursor = *cursor.page_pin;
    *cursor.page_pin = old_cursor;
    cursorChangePin(new_cursor);
    return PageList::SplitError::none;
}

inline PageList::IncreaseCapacityError Screen::appendGrapheme(Cell *cell, uint32_t cp) {
    struct Guard {
        Screen *s;
        ~Guard() { s->cursor.page_pin->node->page()->assertIntegrity(); }
    } guard = {this};

    if (cursor.page_pin->node->page()->appendGrapheme(cursor.page_row, cell, cp) == page::PageError::none)
        return PageList::IncreaseCapacityError::none;

    /* OutOfMemory:
     * We need to determine the actual cell index of the cell so
     * that after we adjust the capacity we can reload the cell. */
    Cell *zero = cursor.page_cell - cursor.x;
    const size_t cell_idx = (size_t)(cell - zero);

    /* Adjust our capacity. This will update our cursor page pin and
     * force us to reload. */
    PageList::Node *n;
    const PageList::IncreaseCapacityError e =
        increaseCapacity(cursor.page_pin->node, PageList::IncreaseCapacity::grapheme_bytes, &n);
    if (e != PageList::IncreaseCapacityError::none) return e;

    /* The cell pointer is now invalid, so we need to get it from
     * the reloaded cursor pointers. */
    Cell *reloaded_cell;
    if (cell_idx == cursor.x) {
        reloaded_cell = cursor.page_cell;
    } else if (cell_idx < cursor.x) {
        reloaded_cell = cursorCellLeft((size::CellCountInt)(cursor.x - cell_idx));
    } else {
        reloaded_cell = cursorCellRight((size::CellCountInt)(cell_idx - cursor.x));
    }

    if (cursor.page_pin->node->page()->appendGrapheme(cursor.page_row, reloaded_cell, cp) != page::PageError::none) {
        /* This should never happen because we just increased capacity.
         * Log loudly but still return an error so we don't just
         * crash.
         * log.err("grapheme append failed after capacity increase") */
        return PageList::IncreaseCapacityError::OutOfMemory;
    }
    return PageList::IncreaseCapacityError::none;
}

inline PageList::IncreaseCapacityError Screen::startHyperlink(const uint8_t *uri, size_t uri_len,
                                                              const uint8_t *id, size_t id_len) {
    /* Create our pending entry. */
    hyperlink::Hyperlink link;
    link.uri = uri;
    link.uri_len = uri_len;
    if (id) {
        link.id = hyperlink::Hyperlink::Id::makeExplicit(id, id_len);
    } else {
        link.id = hyperlink::Hyperlink::Id::makeImplicit(cursor.hyperlink_implicit_id);
        cursor.hyperlink_implicit_id += 1;
    }
    auto errdefer = [&]() {
        if (link.id.tag == hyperlink::Hyperlink::Id::Tag::implicit) cursor.hyperlink_implicit_id -= 1;
    };

    /* Loop until we have enough page memory to add the hyperlink */
    for (;;) {
        const page::PageError err = startHyperlinkOnce(link);
        if (err == page::PageError::none) return PageList::IncreaseCapacityError::none;

        Maybe<PageList::IncreaseCapacity> adj;
        switch (err) {
        /* An actual self.alloc OOM is a fatal error. */
        case page::PageError::OutOfMemory: errdefer(); return PageList::IncreaseCapacityError::OutOfMemory;

        /* strings table is out of memory, adjust it up */
        case page::PageError::StringsOutOfMemory: adj = PageList::IncreaseCapacity::string_bytes; break;

        /* hyperlink set is out of memory, adjust it up */
        case page::PageError::SetOutOfMemory: adj = PageList::IncreaseCapacity::hyperlink_bytes; break;

        /* hyperlink set is too full, rehash it */
        default: break;
        }

        PageList::Node *n;
        const PageList::IncreaseCapacityError e = increaseCapacity(cursor.page_pin->node, adj, &n);
        if (e != PageList::IncreaseCapacityError::none) {
            errdefer();
            return e;
        }

        assertIntegrity();
    }
}

inline page::PageError Screen::startHyperlinkOnce(const hyperlink::Hyperlink &source) {
    /* Allocate our new Hyperlink entry in non-page memory. This
     * lets us quickly get access to URI, ID. */
    hyperlink::Hyperlink *link = alloc.create<hyperlink::Hyperlink>();
    if (!link) return page::PageError::OutOfMemory;
    if (!source.dupe(alloc, link)) {
        alloc.destroy(link);
        return page::PageError::OutOfMemory;
    }

    /* End any prior hyperlink only after duplicating the new value. The
     * source slices are allowed to reference our current hyperlink. */
    endHyperlink();

    /* Insert the hyperlink into page memory */
    Page *page = cursor.page_pin->node->page();
    hyperlink::Id id;
    const page::PageError e = page->insertHyperlink(*link, &id);
    if (e != page::PageError::none) {
        link->deinit(alloc);
        alloc.destroy(link);
        return e;
    }

    /* Save it all */
    cursor.hyperlink = link;
    cursor.hyperlink_id = id;
    return page::PageError::none;
}

inline PageList::IncreaseCapacityError Screen::cursorSetHyperlink() {
    assert(cursor.hyperlink_id != 0);

    Page *page = cursor.page_pin->node->page();
    const page::PageError err = page->setHyperlink(cursor.page_row, cursor.page_cell, cursor.hyperlink_id);
    if (err == page::PageError::none) {
        /* Success, increase the refcount for the hyperlink. */
        page->hyperlink_set.use((const void *)page->memory, cursor.hyperlink_id);
        return PageList::IncreaseCapacityError::none;
    }

    /* hyperlink_map is out of space, realloc the page to be larger
     *
     * Attempt to allocate the space that would be required to
     * insert a new copy of the cursor hyperlink uri in to the
     * string alloc, since right now increaseCapacity always just
     * adds an extra copy even if one already exists in the page.
     * If this alloc fails then we know we also need to grow our
     * string bytes.
     *
     * FIXME: increaseCapacity should not do this. */
    while (hyperlink::Hyperlink *link = cursor.hyperlink) {
        uint8_t *slice;
        if (page->string_alloc.alloc<uint8_t>((const void *)page->memory, link->uri_len, &slice)) {
            /* We don't bother freeing because we're
             * about to free the entire page anyway. */
            break;
        }

        /* We didn't have enough room, let's increase string bytes */
        PageList::Node *new_node;
        const PageList::IncreaseCapacityError e =
            increaseCapacity(cursor.page_pin->node, PageList::IncreaseCapacity::string_bytes, &new_node);
        if (e != PageList::IncreaseCapacityError::none) return e;
        assert(new_node == cursor.page_pin->node);
        page = new_node->page();
    }

    /* The hyperlink map is fixed-capacity, so reaching this error
     * means live entries fill the usable map capacity and the page
     * must grow. */
    PageList::Node *n;
    const PageList::IncreaseCapacityError e =
        increaseCapacity(cursor.page_pin->node, PageList::IncreaseCapacity::hyperlink_bytes, &n);
    if (e != PageList::IncreaseCapacityError::none) return e;

    /* Retry
     *
     * We check that the cursor hyperlink hasn't been destroyed
     * by the capacity adjustment first though- since despite the
     * terrible code above, that can still apparently happen ._. */
    if (cursor.hyperlink_id > 0) {
        return cursorSetHyperlink();
    }
    return PageList::IncreaseCapacityError::none;
}

/* selection_codepoints.zig: default_line_whitespace */
namespace selection_codepoints {
static const uint32_t default_line_whitespace[] = {0, ' ', '\t'};
}

inline Screen::SelectLine::SelectLine(const Pin &p)
    : pin(p), whitespace(selection_codepoints::default_line_whitespace), whitespace_len(3),
      semantic_prompt_boundary(true) {}

namespace screen_detail {
inline bool indexOfScalar(const uint32_t *hay, size_t len, uint32_t v) {
    for (size_t i = 0; i < len; i++)
        if (hay[i] == v) return true;
    return false;
}
} /* namespace screen_detail */

inline Maybe<Selection> Screen::selectLine(const SelectLine &opts) const {
    /* Get the current point semantic prompt state since that determines
     * boundary conditions too. This makes it so that line selection can
     * only happen within the same prompt state. For example, if you triple
     * click output, but the shell uses spaces to soft-wrap to the prompt
     * then the selection will stop prior to the prompt. See issue #1329. */
    Maybe<Cell::SemanticContent> semantic_prompt_state;
    if (opts.semantic_prompt_boundary) {
        semantic_prompt_state = opts.pin.rowAndCell().cell->semantic_content();
    }

    /* The real start of the row is the first row in the soft-wrap. */
    Pin start_pin;
    do {
        PageList::RowIterator it = opts.pin.rowIterator(PageList::Direction::left_up, Maybe<Pin>());
        Pin it_prev;
        (void)it.next(&it_prev); /* skip self */

        /* First, check the current row for semantic boundaries before the clicked position. */
        if (semantic_prompt_state.has) {
            const Cell::SemanticContent v = semantic_prompt_state.value;
            const Row *row = it_prev.rowAndCell().row;
            const Cell *cells = it_prev.node->page()->getCells(row);
            /* Scan backwards from clicked position to find where our content starts */
            bool found = false;
            for (size_t i = 0; i < (size_t)opts.pin.x + 1; i++) {
                const size_t x_rev = opts.pin.x - i;
                if (cells[x_rev].semantic_content() != v) {
                    Pin copy = it_prev;
                    copy.x = (size::CellCountInt)(x_rev + 1);
                    start_pin = copy;
                    found = true;
                    break;
                }
            }
            if (found) break;

            /* No boundary found before clicked position on current row.
             * If row doesn't wrap from above, start is at column 0.
             * Otherwise, continue checking previous rows. */
        }

        bool done = false;
        Pin p;
        while (it.next(&p)) {
            const Row *row = p.rowAndCell().row;

            if (!row->wrap()) {
                Pin copy = it_prev;
                copy.x = 0;
                start_pin = copy;
                done = true;
                break;
            }

            if (semantic_prompt_state.has) {
                const Cell::SemanticContent v = semantic_prompt_state.value;
                /* We need to check every cell in this row in reverse
                 * order since we're going up and back. */
                const Cell *cells = p.node->page()->getCells(row);
                const size_t len = p.node->page()->size.cols;
                bool boundary = false;
                for (size_t x = 0; x < len; x++) {
                    const size_t x_rev = len - 1 - x;
                    if (cells[x_rev].semantic_content() != v) {
                        start_pin = it_prev;
                        boundary = true;
                        break;
                    }
                    it_prev = p;
                    it_prev.x = (size::CellCountInt)x_rev;
                }
                if (boundary) {
                    done = true;
                    break;
                }

                continue;
            }

            it_prev = p;
        }
        if (!done) {
            Pin copy = it_prev;
            copy.x = 0;
            start_pin = copy;
        }
    } while (false);

    /* The real end of the row is the final row in the soft-wrap. */
    Pin end_pin;
    {
        bool found = false;
        PageList::RowIterator it = opts.pin.rowIterator(PageList::Direction::right_down, Maybe<Pin>());
        Pin p;
        while (!found && it.next(&p)) {
            const Row *row = p.rowAndCell().row;

            if (semantic_prompt_state.has) {
                const Cell::SemanticContent v = semantic_prompt_state.value;
                /* We need to check every cell in this row */
                const Cell *cells = p.node->page()->getCells(row);
                const size_t len = p.node->page()->size.cols;

                /* If this is our pin row we can start from our x because
                 * the start_pin logic already found the real start. */
                const size_t start_offset = (p.node == opts.pin.node && p.y == opts.pin.y) ? opts.pin.x : 0;

                /* Handle the zero case specially because if the first
                 * col doesn't match then we end at the end of the prior
                 * row. But if this is the first row, we can't go back,
                 * so we scan forward to find where our content ends. */
                if (start_offset == 0 && cells[0].semantic_content() != v) {
                    Pin prev = p.up(1).value;
                    prev.x = (size::CellCountInt)(prev.node->cols() - 1);
                    end_pin = prev;
                    found = true;
                    break;
                }

                /* For every other case, we end at the prior cell. */
                for (size_t x = start_offset; x < len; x++) {
                    if (cells[x].semantic_content() != v) {
                        Pin copy = p;
                        copy.x = (size::CellCountInt)(x - 1);
                        end_pin = copy;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }

            if (!row->wrap()) {
                Pin copy = p;
                copy.x = (size::CellCountInt)(p.node->cols() - 1);
                end_pin = copy;
                found = true;
                break;
            }
        }

        if (!found) return Maybe<Selection>::none();
    }

    /* Go forward from the start to find the first non-whitespace character. */
    Pin start;
    if (opts.whitespace_len == SIZE_MAX) {
        start = start_pin;
    } else {
        bool found = false;
        PageList::CellIterator it = start_pin.cellIterator(PageList::Direction::right_down, end_pin);
        Pin p;
        while (it.next(&p)) {
            const Cell *cell = p.rowAndCell().cell;
            if (!cell->hasText()) continue;

            /* Non-empty means we found it. */
            if (screen_detail::indexOfScalar(opts.whitespace, opts.whitespace_len, cell->contentCodepoint())) continue;

            start = p;
            found = true;
            break;
        }

        if (!found) return Maybe<Selection>::none();
    }

    /* Go backward from the end to find the first non-whitespace character. */
    Pin end;
    if (opts.whitespace_len == SIZE_MAX) {
        end = end_pin;
    } else {
        bool found = false;
        PageList::CellIterator it = end_pin.cellIterator(PageList::Direction::left_up, start_pin);
        Pin p;
        while (it.next(&p)) {
            const Cell *cell = p.rowAndCell().cell;
            if (!cell->hasText()) continue;

            /* Non-empty means we found it. */
            if (screen_detail::indexOfScalar(opts.whitespace, opts.whitespace_len, cell->contentCodepoint())) continue;

            end = p;
            found = true;
            break;
        }

        if (!found) return Maybe<Selection>::none();
    }

    return Selection::init(start, end, false);
}

inline Maybe<Selection> Screen::selectAll() {
    const uint32_t whitespace[] = {0, ' ', '\t'};

    Pin start;
    {
        bool found = false;
        PageList::CellIterator it = pages.cellIterator(PageList::Direction::right_down, point::Point::screen());
        Pin p;
        while (it.next(&p)) {
            const Cell *cell = p.rowAndCell().cell;
            if (!cell->hasText()) continue;

            /* Non-empty means we found it. */
            if (screen_detail::indexOfScalar(whitespace, 3, cell->contentCodepoint())) continue;

            start = p;
            found = true;
            break;
        }

        if (!found) return Maybe<Selection>::none();
    }

    Pin end;
    {
        bool found = false;
        PageList::CellIterator it = pages.cellIterator(PageList::Direction::left_up, point::Point::screen());
        Pin p;
        while (it.next(&p)) {
            const Cell *cell = p.rowAndCell().cell;
            if (!cell->hasText()) continue;

            /* Non-empty means we found it. */
            if (screen_detail::indexOfScalar(whitespace, 3, cell->contentCodepoint())) continue;

            end = p;
            found = true;
            break;
        }

        if (!found) return Maybe<Selection>::none();
    }

    return Selection::init(start, end, false);
}

inline Maybe<Selection> Screen::selectWord(const Pin &pin, const uint32_t *boundary_codepoints,
                                           size_t boundary_len) {
    /* If our cell is empty we can't select a word, because we can't select
     * areas where the screen is not yet written. */
    const Cell *start_cell = pin.rowAndCell().cell;
    if (!start_cell->hasText()) return Maybe<Selection>::none();

    /* Determine if we are a boundary or not to determine what our boundary is. */
    const bool expect_boundary =
        screen_detail::indexOfScalar(boundary_codepoints, boundary_len, start_cell->contentCodepoint());

    /* Go forwards to find our end boundary */
    Pin end;
    {
        PageList::CellIterator it = pin.cellIterator(PageList::Direction::right_down, Maybe<Pin>());
        Pin prev;
        (void)it.next(&prev); /* Consume one, our start */
        Pin p;
        bool done = false;
        while (it.next(&p)) {
            const Page::RowAndCell rac = p.rowAndCell();
            const Cell *cell = rac.cell;

            /* If we reached an empty cell its always a boundary */
            if (!cell->hasText()) {
                end = prev;
                done = true;
                break;
            }

            /* If we do not match our expected set, we hit a boundary */
            const bool this_boundary =
                screen_detail::indexOfScalar(boundary_codepoints, boundary_len, cell->contentCodepoint());
            if (this_boundary != expect_boundary) {
                end = prev;
                done = true;
                break;
            }

            /* If we are going to the next row and it isn't wrapped, we
             * return the previous. */
            if (p.x == p.node->cols() - 1 && !rac.row->wrap()) {
                end = p;
                done = true;
                break;
            }

            prev = p;
        }

        if (!done) end = prev;
    }

    /* Go backwards to find our start boundary */
    Pin start;
    {
        PageList::CellIterator it = pin.cellIterator(PageList::Direction::left_up, Maybe<Pin>());
        Pin prev;
        (void)it.next(&prev); /* Consume one, our start */
        Pin p;
        bool done = false;
        while (it.next(&p)) {
            const Page::RowAndCell rac = p.rowAndCell();
            const Cell *cell = rac.cell;

            /* If we are going to the next row and it isn't wrapped, we
             * return the previous. */
            if (p.x == p.node->cols() - 1 && !rac.row->wrap()) {
                start = prev;
                done = true;
                break;
            }

            /* If we reached an empty cell its always a boundary */
            if (!cell->hasText()) {
                start = prev;
                done = true;
                break;
            }

            /* If we do not match our expected set, we hit a boundary */
            const bool this_boundary =
                screen_detail::indexOfScalar(boundary_codepoints, boundary_len, cell->contentCodepoint());
            if (this_boundary != expect_boundary) {
                start = prev;
                done = true;
                break;
            }

            prev = p;
        }

        if (!done) start = prev;
    }

    return Selection::init(start, end, false);
}

inline Maybe<Selection> Screen::selectOutput(const Pin &pin) {
    /* If our pin right now is not on output, then we return nothing. */
    if (pin.rowAndCell().cell->semantic_content() != Cell::SemanticContent::output) return Maybe<Selection>::none();

    /* Get the post prior prompt from this pin. This is the prompt whose
     * output we'll be capturing. */
    Pin prompt_pin;
    {
        /* If we have a prompt above this point (including this point),
         * then thats the prompt we want to capture output from. */
        PageList::PromptIterator it = pin.promptIterator(PageList::Direction::left_up, Maybe<Pin>());
        if (!it.next(&prompt_pin)) {
            /* If we don't have a prompt, then we assume that we're
             * capturing all the output up to the next prompt. */
            it = pin.promptIterator(PageList::Direction::right_down, Maybe<Pin>());
            Pin next;
            if (!it.next(&next)) return Maybe<Selection>::none();

            /* We'll capture from the start of the screen to just above
             * the prompt and will trim the trailing whitespace. */
            const Pin start_pin = pages.getTopLeft(point::Tag::screen);
            const Maybe<Pin> up = next.up(1);
            if (!up.has) return Maybe<Selection>::none();
            Pin end_pin = up.value;
            end_pin.x = (size::CellCountInt)(end_pin.node->cols() - 1);
            PageList::CellIterator cell_it = end_pin.cellIterator(PageList::Direction::left_up, start_pin);
            Pin p;
            while (cell_it.next(&p)) {
                const Cell *cell = p.rowAndCell().cell;
                end_pin = p;
                if (cell->hasText()) break;
            }

            return Selection::init(start_pin, end_pin, false);
        }
    }

    /* Grab our content */
    Maybe<PageList::HighlightUntracked> hl_ = pages.highlightSemanticContent(prompt_pin, Cell::SemanticContent::output);
    if (!hl_.has) return Maybe<Selection>::none();
    PageList::HighlightUntracked hl = hl_.value;

    /* Trim our trailing whitespace */
    PageList::CellIterator cell_it = hl.end.cellIterator(PageList::Direction::left_up, hl.start);
    Pin p;
    while (cell_it.next(&p)) {
        const Cell *cell = p.rowAndCell().cell;
        hl.end = p;
        if (cell->hasText()) break;
    }

    return Selection::init(hl.start, hl.end, false);
}

inline bool Screen::LineIterator::next(Selection *out) {
    if (!current.has) return false;
    SelectLine opts(current.value);
    opts.whitespace = nullptr;
    opts.whitespace_len = SIZE_MAX;
    opts.semantic_prompt_boundary = false;
    const Maybe<Selection> result = screen->selectLine(opts);
    if (!result.has) {
        current = Maybe<Pin>::none();
        return false;
    }

    current = result.value.end().down(1);
    *out = result.value;
    return true;
}

inline Screen::PromptClickMove Screen::promptClickLine(const Pin &click_pin) {
    /* If our click pin is our cursor pin, no movement is needed.
     * Do this early so we can assume later that they are different. */
    const Pin cursor_pin = *cursor.page_pin;
    if (cursor_pin.eql(click_pin)) return PromptClickMove::zero();

    /* If our cursor is before our click, we're only emitting right inputs. */
    if (cursor_pin.before(click_pin)) {
        size_t count = 0;

        /* We go row-by-row because soft-wrapped rows are still a single
         * line to a shell, so we can't just look at our page row. */
        PageList::RowIterator row_it = cursor_pin.rowIterator(PageList::Direction::right_down, click_pin);
        Pin row_pin;
        bool done = false;
        while (!done && row_it.next(&row_pin)) {
            const Page::RowAndCell rac = row_pin.rowAndCell();
            const Cell *cells = row_pin.node->page()->getCells(rac.row);
            const size_t len = row_pin.node->page()->size.cols;

            /* Determine if this row is our cursor. */
            const bool is_cursor_row = row_pin.node == cursor_pin.node && row_pin.y == cursor_pin.y;

            /* If this is not the cursor row, verify it's still part of the
             * continuation of our starting prompt. */
            if (!is_cursor_row && rac.row->semantic_prompt() != Row::SemanticPrompt::prompt_continuation) break;

            /* Determine where our input starts. */
            size_t start_x;
            if (is_cursor_row) {
                /* If this is our cursor row then we start after the cursor. */
                start_x = (size_t)cursor_pin.x + 1;
            } else {
                /* Otherwise, we start at the first input cell, because
                 * we expect the shell to properly translate arrows across
                 * lines to the start of the input. Some shells indent
                 * where input starts on subsequent lines so we must do
                 * this. */
                start_x = len;
                for (size_t x = 0; x < len; x++) {
                    if (cells[x].semantic_content() == Cell::SemanticContent::input) {
                        start_x = x;
                        break;
                    }
                }
                /* We never found an input cell, so we need to move to the
                 * next row. */
            }

            /* Iterate over the input cells and assume arrow keys only
             * jump to input cells. */
            for (size_t x = start_x; x < len; x++) {
                /* Ignore non-input cells, but allow breaks. We assume
                 * the shell will translate arrow keys to only input
                 * areas. */
                if (cells[x].semantic_content() != Cell::SemanticContent::input) continue;

                /* Increment our input count */
                count += 1;

                /* If this is our target, we're done. */
                if (row_pin.node == click_pin.node && row_pin.y == click_pin.y && x == click_pin.x) {
                    done = true;
                    break;
                }
            }
            if (done) break;

            /* If this row isn't soft-wrapped, we need to break out
             * because line based moving only handles single lines.
             * We're done! */
            if (!rac.row->wrap()) {
                /* If we never found our pin, that means we clicked further
                 * right/beyond it. If we're already on a non-empty input cell
                 * then we add one so we can move to the newest, empty cell
                 * at the end, matching typical editor behavior. */
                if (cursor.page_cell->semantic_content() == Cell::SemanticContent::input) count += 1;

                break;
            }
        }

        PromptClickMove m = {0, count};
        return m;
    }

    /* Otherwise, cursor is after click, so we're emitting left inputs. */
    size_t count = 0;

    /* We go row-by-row because soft-wrapped rows are still a single
     * line to a shell, so we can't just look at our page row. */
    PageList::RowIterator row_it = cursor_pin.rowIterator(PageList::Direction::left_up, click_pin);
    Pin row_pin;
    bool done = false;
    while (!done && row_it.next(&row_pin)) {
        const Page::RowAndCell rac = row_pin.rowAndCell();
        const Cell *cells = row_pin.node->page()->getCells(rac.row);

        /* Determine the length of the cells we look at in this row. */
        size_t end_len;
        if (row_pin.node == cursor_pin.node && row_pin.y == cursor_pin.y) {
            /* If this is our cursor row then we end before the cursor. */
            end_len = cursor_pin.x;
        } else {
            /* Otherwise, we end at the last cell in the row. */
            end_len = row_pin.node->page()->size.cols;
        }

        /* Iterate backwards over the input cells. */
        for (size_t rev_x = 0; rev_x < end_len; rev_x++) {
            const size_t x = end_len - 1 - rev_x;

            /* Ignore non-input cells. */
            if (cells[x].semantic_content() != Cell::SemanticContent::input) continue;

            /* Increment our input count */
            count += 1;

            /* If this is our target, we're done. */
            if (row_pin.node == click_pin.node && row_pin.y == click_pin.y && x == click_pin.x) {
                done = true;
                break;
            }
        }
        if (done) break;

        /* If this row is not a wrap continuation, then break out */
        if (!rac.row->wrap_continuation()) break;
    }

    PromptClickMove m = {count, 0};
    return m;
}

inline bool Screen::testWriteString(const char *text) {
    const uint8_t *s = (const uint8_t *)text;
    size_t i = 0;
    const size_t n = strlen(text);
    while (i < n) {
        /* Wisp: std.unicode.Utf8View iterator. */
        uint32_t c;
        const uint8_t c0 = s[i];
        size_t len;
        if (c0 < 0x80) {
            c = c0;
            len = 1;
        } else if ((c0 >> 5) == 6) {
            c = c0 & 0x1F;
            len = 2;
        } else if ((c0 >> 4) == 14) {
            c = c0 & 0x0F;
            len = 3;
        } else {
            c = c0 & 0x07;
            len = 4;
        }
        for (size_t k = 1; k < len; k++) c = (c << 6) | (s[i + k] & 0x3F);
        i += len;

        /* Explicit newline forces a new row */
        if (c == '\n') {
            if (!cursorDownOrScroll()) return false;
            cursorHorizontalAbsolute(0);
            cursor.pending_wrap = false;
            if (cursor.semantic_content_clear_eol) {
                cursorSetSemanticContent(SemanticContentSet::makeOutput());
            } else if (cursor.semantic_content != Cell::SemanticContent::output) {
                cursor.page_row->setSemanticPrompt(Row::SemanticPrompt::prompt_continuation);
            }
            continue;
        }

        const size_t width = c <= 0xFF ? 1 : (size_t)unicode::Table::get(c).width();
        if (width == 0) {
            Cell *cell = cursorCellLeft(1);
            switch (cell->wide()) {
            case Cell::Wide::narrow:
            case Cell::Wide::wide: break;
            case Cell::Wide::spacer_head: assert(false); break;
            case Cell::Wide::spacer_tail: cell = cursorCellLeft(2); break;
            }

            if (cursor.page_pin->node->page()->appendGrapheme(cursor.page_row, cell, c) != page::PageError::none)
                return false;
            continue;
        }

        if (cursor.pending_wrap) {
            assert(cursor.x == pages.cols - 1);
            cursor.pending_wrap = false;
            cursor.page_row->setWrap(true);
            if (!cursorDownOrScroll()) return false;
            cursorHorizontalAbsolute(0);
            cursor.page_row->setWrapContinuation(true);
            if (cursor.semantic_content != Cell::SemanticContent::output) {
                cursor.page_row->setSemanticPrompt(Row::SemanticPrompt::prompt_continuation);
            }
        }

        assert(width == 1 || width == 2);
        if (width == 1) {
            Cell cell = Cell::init(c);
            cell.setStyleId(cursor.style_id);
            cell.setProtected(cursor.protected_);
            cell.setSemanticContent(cursor.semantic_content);
            *cursor.page_cell = cell;

            /* If we have a ref-counted style, increase. */
            if (cursor.style_id != style::default_id) {
                Page *page = cursor.page_pin->node->page();
                page->styles.use((const void *)page->memory, cursor.style_id);
                cursor.page_row->setStyled(true);
            }

            /* If we have a hyperlink, add it to the cell. */
            if (cursor.hyperlink_id > 0 && cursorSetHyperlink() != PageList::IncreaseCapacityError::none) return false;
        } else {
            /* Need a wide spacer head */
            if (cursor.x == pages.cols - 1) {
                Cell head = Cell::init(0);
                head.setWide(Cell::Wide::spacer_head);
                head.setProtected(cursor.protected_);
                head.setSemanticContent(cursor.semantic_content);
                *cursor.page_cell = head;

                /* If we have a hyperlink, add it to the cell. */
                if (cursor.hyperlink_id > 0 && cursorSetHyperlink() != PageList::IncreaseCapacityError::none)
                    return false;

                cursor.page_row->setWrap(true);
                if (!cursorDownOrScroll()) return false;
                cursorHorizontalAbsolute(0);
                cursor.page_row->setWrapContinuation(true);
            }

            /* Write our wide char */
            Cell wide = Cell::init(c);
            wide.setStyleId(cursor.style_id);
            wide.setWide(Cell::Wide::wide);
            wide.setProtected(cursor.protected_);
            wide.setSemanticContent(cursor.semantic_content);
            *cursor.page_cell = wide;

            /* If we have a hyperlink, add it to the cell. */
            if (cursor.hyperlink_id > 0 && cursorSetHyperlink() != PageList::IncreaseCapacityError::none) return false;

            /* Write our tail */
            cursorRight(1);
            Cell tail = Cell::init(0);
            tail.setWide(Cell::Wide::spacer_tail);
            tail.setProtected(cursor.protected_);
            tail.setSemanticContent(cursor.semantic_content);
            *cursor.page_cell = tail;

            /* If we have a hyperlink, add it to the cell. */
            if (cursor.hyperlink_id > 0 && cursorSetHyperlink() != PageList::IncreaseCapacityError::none) return false;

            /* If we have a ref-counted style, increase twice. */
            if (cursor.style_id != style::default_id) {
                Page *page = cursor.page_pin->node->page();
                page->styles.use((const void *)page->memory, cursor.style_id);
                page->styles.use((const void *)page->memory, cursor.style_id);
                cursor.page_row->setStyled(true);
            }
        }

        if ((size_t)cursor.x + 1 < pages.cols) {
            cursorRight(1);
        } else {
            cursor.pending_wrap = true;
        }
    }
    return true;
}

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SCREEN_IMPL_HPP */
