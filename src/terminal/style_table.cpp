/* Interned style table for the screen buffer. See style_table.h.
 *
 * This file is Wisp's own code. It adapts the ported style::Set — which is a
 * port of Ghostty's ref_counted_set.zig, see THIRD_PARTY_NOTICES.md — to the
 * needs of a flat screen buffer rather than a bounded page.
 */

#include <stdlib.h>
#include <string.h>

#include "style_table.h"
#include "style.hpp"
#include "../core/log.h"

/* wingdi.h, reached through windows.h by both style_table.h and log.h,
 * defines RGB(r,g,b) as a macro. That silently rewrites every RGB(...)
 * constructor call below into a COLORREF. The undef has to come after every
 * include, since any later one can pull windows.h back in and redefine it. */
#ifdef RGB
#undef RGB
#endif

using namespace wisp::terminal;

/* Room for a few hundred distinct styles covers ordinary terminal output.
 * Truecolor-heavy output blows straight past it, which is what growth is
 * for. */
static const uint16_t INITIAL_STYLES = 256;

/* A screen cell's style ID is 16 bits, so the table cannot exceed what an ID
 * can name. Past this point new styles are refused and those cells render
 * unstyled, which is a visible degradation but never wrong styling. */
static const uint32_t MAX_STYLES = 0xFFFE;

struct StyleTable {
    uint8_t    *memory;
    size_t      size;
    style::Set  set;
    uint16_t    capacity;
};

/* ─── attribute mapping ──────────────────────────────────────────────────── */

/* CellAttr has no notion of "unset", so every color maps to an explicit
 * palette or rgb value. That keeps the round trip exact: resolve(intern(a))
 * equals a for every a. */

static style::Style attr_to_style(const CellAttr &a) {
    style::Style s;

    s.fg_color = (a.fg_idx == 0xFF)
        ? style::StyleColor::from_rgb(RGB((uint8_t)(a.fg_rgb >> 16),
                                          (uint8_t)(a.fg_rgb >> 8),
                                          (uint8_t)(a.fg_rgb)))
        : style::StyleColor::from_palette(a.fg_idx);

    s.bg_color = (a.bg_idx == 0xFF)
        ? style::StyleColor::from_rgb(RGB((uint8_t)(a.bg_rgb >> 16),
                                          (uint8_t)(a.bg_rgb >> 8),
                                          (uint8_t)(a.bg_rgb)))
        : style::StyleColor::from_palette(a.bg_idx);

    s.flags.bold = a.bold != 0;
    s.flags.italic = a.italic != 0;
    s.flags.blink = a.blink != 0;
    s.flags.inverse = a.reverse != 0;
    s.flags.strikethrough = a.strikethrough != 0;

    /* The screen buffer's "dim" is SGR 2, which the ported style calls
     * faint. */
    s.flags.faint = a.dim != 0;

    /* The screen buffer only tracks underline on or off, with no style, so
     * anything underlined maps to single. */
    s.flags.underline = a.underline ? Underline::single : Underline::none;

    return s;
}

static CellAttr style_to_attr(const style::Style &s) {
    CellAttr a;
    memset(&a, 0, sizeof(a));

    if (s.fg_color.tag == style::StyleColor::Tag::rgb) {
        a.fg_idx = 0xFF;
        a.fg_rgb = ((uint32_t)s.fg_color.rgb.r << 16) |
                   ((uint32_t)s.fg_color.rgb.g << 8) |
                   (uint32_t)s.fg_color.rgb.b;
    } else {
        a.fg_idx = s.fg_color.palette;
    }

    if (s.bg_color.tag == style::StyleColor::Tag::rgb) {
        a.bg_idx = 0xFF;
        a.bg_rgb = ((uint32_t)s.bg_color.rgb.r << 16) |
                   ((uint32_t)s.bg_color.rgb.g << 8) |
                   (uint32_t)s.bg_color.rgb.b;
    } else {
        a.bg_idx = s.bg_color.palette;
    }

    a.bold = s.flags.bold ? 1 : 0;
    a.italic = s.flags.italic ? 1 : 0;
    a.blink = s.flags.blink ? 1 : 0;
    a.reverse = s.flags.inverse ? 1 : 0;
    a.strikethrough = s.flags.strikethrough ? 1 : 0;
    a.dim = s.flags.faint ? 1 : 0;
    a.underline = (s.flags.underline != Underline::none) ? 1 : 0;

    return a;
}

extern "C" CellAttr style_table_default_attr(void) {
    /* Matches the old screen_cell_blank: white on black. */
    CellAttr a;
    memset(&a, 0, sizeof(a));
    a.fg_idx = 7;
    a.bg_idx = 0;
    return a;
}

/* The default attributes as a Style, computed once. Anything equal to it
 * interns to ID 0 and costs nothing. */
static style::Style default_style() {
    return attr_to_style(style_table_default_attr());
}

/* ─── lifetime ───────────────────────────────────────────────────────────── */

static bool table_alloc(StyleTable *t, uint16_t styles) {
    const style::Set::Layout l =
        style::Set::Layout::init(style::Set::capacity_for_count(styles));

    uint8_t *mem = (uint8_t *)calloc(1, l.total_size ? l.total_size : 1);
    if (!mem) return false;

    t->memory = mem;
    t->size = l.total_size;
    t->capacity = styles;
    /* calloc zeroed it, which is exactly the set's empty state. */
    t->set = style::Set::init_assume_zeroed(OffsetBuf::init(mem), l, style::Context());
    return true;
}

extern "C" StyleTable *style_table_create(void) {
    StyleTable *t = (StyleTable *)calloc(1, sizeof(StyleTable));
    if (!t) return NULL;

    if (!table_alloc(t, INITIAL_STYLES)) {
        free(t);
        return NULL;
    }
    return t;
}

extern "C" void style_table_destroy(StyleTable *t) {
    if (!t) return;
    free(t->memory);
    free(t);
}

/* ─── growth ─────────────────────────────────────────────────────────────── */

/* Rebuild the table at a larger capacity, preserving every live style's ID
 * and reference count.
 *
 * Preserving IDs is the whole difficulty. Cells across the scrollback already
 * hold IDs; if a rebuild reassigned them, every one of those cells would point
 * at the wrong style. add_with_id lets each entry be reinstated under the ID
 * it already had.
 *
 * Returns false if the table cannot grow, leaving the existing one untouched
 * so the caller degrades rather than losing data. */
static bool table_grow(StyleTable *t) {
    if (t->capacity >= MAX_STYLES) return false;

    uint32_t want = (uint32_t)t->capacity * 2;
    if (want > MAX_STYLES) want = MAX_STYLES;

    StyleTable next;
    memset(&next, 0, sizeof(next));
    if (!table_alloc(&next, (uint16_t)want)) return false;

    /* Reinstate every live entry under its original ID. */
    style::Set::Iterator it = t->set.iterator(t->memory);
    style::Id id;
    style::Style *value;
    while (it.next(&id, &value)) {
        const uint16_t refs = t->set.ref_count(t->memory, id);
        if (refs == 0) continue;

        style::Id got = 0;
        if (next.set.add_with_id(next.memory, *value, id, &got) != AddResult::ok ||
            got != id) {
            /* Could not reproduce the ID map, so abandon the rebuild rather
             * than install a table that renames styles under live cells. */
            free(next.memory);
            return false;
        }

        /* add_with_id took one reference; restore the rest. */
        for (uint16_t i = 1; i < refs; i++) next.set.use(next.memory, id);
    }

    free(t->memory);
    t->memory = next.memory;
    t->size = next.size;
    t->set = next.set;
    t->capacity = next.capacity;

    WISP_LOG_DEBUG("style table grew to %u styles", (unsigned)t->capacity);
    return true;
}

/* ─── operations ─────────────────────────────────────────────────────────── */

extern "C" uint16_t style_table_intern(StyleTable *t, CellAttr attr, bool *ok) {
    if (ok) *ok = true;
    if (!t) { if (ok) *ok = false; return 0; }

    const style::Style s = attr_to_style(attr);

    /* The default style is ID 0 and is never stored. */
    if (s.eql(default_style())) return 0;

    style::Id id = 0;
    AddResult r = t->set.add(t->memory, s, &id);

    if (r != AddResult::ok) {
        /* Both out_of_memory and needs_rehash are answered by rebuilding at a
         * larger capacity, which also compacts away dead entries. */
        if (!table_grow(t)) {
            WISP_LOG_WARN("style table full at %u styles; cell renders unstyled",
                          (unsigned)t->capacity);
            if (ok) *ok = false;
            return 0;
        }
        r = t->set.add(t->memory, s, &id);
        if (r != AddResult::ok) {
            if (ok) *ok = false;
            return 0;
        }
    }

    return (uint16_t)id;
}

/* Whether an ID can be safely passed to the set.
 *
 * The set's own accessors assert the ID is in range, so a caller holding a
 * stale or bogus ID must be filtered out here rather than by them. A cell
 * from a discarded scrollback line is the realistic source of one. */
static bool id_is_live(const StyleTable *t, uint16_t id) {
    if (!t || id == 0) return false;
    if ((size_t)id >= t->set.layout.cap) return false;
    return t->set.ref_count(t->memory, id) > 0;
}

extern "C" CellAttr style_table_resolve(const StyleTable *t, uint16_t id) {
    if (!id_is_live(t, id)) return style_table_default_attr();
    return style_to_attr(*t->set.get(t->memory, id));
}

extern "C" void style_table_release(StyleTable *t, uint16_t id) {
    if (!id_is_live(t, id)) return;
    t->set.release(t->memory, id);
}

extern "C" void style_table_use(StyleTable *t, uint16_t id) {
    if (!id_is_live(t, id)) return;
    t->set.use(t->memory, id);
}

extern "C" uint32_t style_table_count(const StyleTable *t) {
    return t ? (uint32_t)t->set.count() : 0;
}

extern "C" uint32_t style_table_capacity(const StyleTable *t) {
    return t ? t->capacity : 0;
}
