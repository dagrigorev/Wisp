/* A C-callable interned style table for the screen buffer.
 *
 * Backed by the ported style::Set (src/terminal/style.hpp), which in turn is
 * a port of Ghostty's ref_counted_set.zig — see THIRD_PARTY_NOTICES.md.
 *
 * WHY THIS EXISTS. A ScreenCell used to carry its full attributes inline,
 * twelve bytes of color and flags per cell. Most cells on a screen share a
 * handful of distinct styles, so the table stores each distinct style once and
 * the cell keeps a two-byte ID.
 *
 * WHY IT GROWS. Ghostty interns styles per page, where the row count bounds
 * how many distinct styles can appear. Wisp's screen buffer is one flat
 * allocation with a long scrollback, so a fixed table would be exhausted by
 * truecolor-heavy output — anything emitting a different color per cell — and
 * those cells would lose their styling. The table therefore grows on demand.
 *
 * IDs ARE STABLE ACROSS GROWTH. Cells hold IDs, so growth re-interns every
 * live style under the ID it already had. If IDs were reassigned, every cell
 * in the scrollback would silently point at the wrong style.
 *
 * ID 0 is the default style and is never stored.
 */

#pragma once
#ifndef WISP_TERMINAL_STYLE_TABLE_H
#define WISP_TERMINAL_STYLE_TABLE_H

#include <stdint.h>
#include "wisp_bool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The attributes a cell can carry. This is the old CellAttr shape, kept as the
 * boundary type so callers in C continue to work in terms of plain values
 * while storage underneath is interned. */
typedef struct {
    uint8_t  fg_idx;      /* 0-255 palette index; 0xFF means use fg_rgb */
    uint8_t  bg_idx;      /* 0-255 palette index; 0xFF means use bg_rgb */
    uint32_t fg_rgb;      /* truecolor, when fg_idx == 0xFF */
    uint32_t bg_rgb;      /* truecolor, when bg_idx == 0xFF */
    uint8_t  bold     : 1;
    uint8_t  italic   : 1;
    uint8_t  underline: 1;
    uint8_t  blink    : 1;
    uint8_t  reverse  : 1;
    uint8_t  dim      : 1;
    uint8_t  strikethrough : 1;
    uint8_t  _pad     : 1;
} CellAttr;

/* The default attributes, which intern to ID 0. */
CellAttr style_table_default_attr(void);

/* Opaque handle. */
typedef struct StyleTable StyleTable;

/* Create a table. Returns NULL on allocation failure. */
StyleTable *style_table_create(void);
void        style_table_destroy(StyleTable *t);

/* Intern attributes and return their ID, taking a reference.
 *
 * Returns 0 for the default attributes without consuming a slot. Returns 0
 * and sets *ok to false if the table could not grow, in which case the cell
 * renders unstyled rather than wrongly styled. Pass NULL for ok to ignore. */
uint16_t style_table_intern(StyleTable *t, CellAttr attr, bool *ok);

/* Resolve an ID back to attributes. An unknown or zero ID yields the
 * default. */
CellAttr style_table_resolve(const StyleTable *t, uint16_t id);

/* Release a reference previously taken by style_table_intern. Releasing ID 0
 * is a no-op. */
void style_table_release(StyleTable *t, uint16_t id);

/* Take an additional reference, for when a cell is copied rather than
 * newly written. */
void style_table_use(StyleTable *t, uint16_t id);

/* Number of distinct styles currently held. Used by tests and diagnostics. */
uint32_t style_table_count(const StyleTable *t);

/* Slots currently allocated, which grows as needed. Tests use this to
 * confirm growth actually happened. */
uint32_t style_table_capacity(const StyleTable *t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WISP_TERMINAL_STYLE_TABLE_H */
