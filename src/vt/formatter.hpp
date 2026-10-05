/* Transliterated from Ghostty src/terminal/formatter.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: std.Io.Writer is std::string (writes can't fail); the writeCellRun
 * fast path is not carried over — it is an output-identical batching of
 * the per-cell path, which this port always takes. TerminalFormatter is
 * ported with Terminal.
 */

#pragma once
#ifndef WISP_VT_FORMATTER_HPP
#define WISP_VT_FORMATTER_HPP

#include <stdio.h>

#include <string>
#include <vector>

#include "fastprint.hpp"
#include "screen.hpp"

namespace wisp {
namespace vt {
namespace formatter {

typedef PageList::Pin Pin;
typedef page::Page Page;
typedef page::Row Row;
typedef page::Cell Cell;
typedef point::Coordinate Coordinate;
typedef style::Style Style;

/* Formats available. */
enum class Format {
    /* Plain text. */
    plain,

    /* Include VT sequences to preserve colors, styles, URLs, etc.
     * This is predominantly SGR sequences but may contain others as needed.
     *
     * Note that for reference colors, like palette indices, this will
     * vary based on the formatter and you should see the docs. For example,
     * PageFormatter with VT will emit SGR sequences with palette indices,
     * not the color itself.
     *
     * For VT, newlines will be emitted as `\r\n` so that the cursor properly
     * moves back to the beginning prior emitting follow-up lines. */
    vt,

    /* HTML output.
     *
     * This will emit inline styles for as much styling as possible,
     * in the interest of simplicity and ease of editing. This isn't meant
     * to build the most beautiful or efficient HTML, but rather to be
     * stylistically correct.
     *
     * For colors, RGB values are emitted as inline CSS (#RRGGBB) while palette
     * indices use CSS variables (var(--vt-palette-N)). The palette colors are
     * emitted by TerminalFormatter.Extra.palette as a <style> block if you
     * want to also include that. But if you only format a screen or lower,
     * the formatter doesn't have access to the current palette to render it.
     *
     * Newlines are emitted as actual '\n' characters. Consumers should use
     * CSS white-space: pre or pre-wrap to preserve spacing and alignment. */
    html,
};

/* Returns true if the format emits styled output (not plaintext). */
inline bool formatStyled(Format fmt) { return fmt != Format::plain; }

struct CodepointMap {
    /* Unicode codepoint range to replace.
     * Asserts: range[0] <= range[1] */
    uint32_t range[2];

    /* Replacement value for this range. */
    struct Replacement {
        enum class Tag { codepoint, string } tag;

        /* A single replacement codepoint. */
        uint32_t codepoint;

        /* A UTF-8 encoded string to replace with. Asserts the
         * UTF-8 encoding (must be valid). */
        std::string string;
    } replacement;
};

/* Common encoding options regardless of what exact formatter is used. */
struct Options {
    /* The format to emit. */
    Format emit;

    /* Whether to unwrap soft-wrapped lines. If false, this will emit the
     * screen contents as it is rendered on the page in the given size. */
    bool unwrap; /* = false */

    /* Trim trailing whitespace on lines with other text. Trailing blank
     * lines are always trimmed. This only affects trailing whitespace
     * on rows that have at least one other cell with text. Whitespace
     * is currently only space characters (0x20). */
    bool trim; /* = true */

    /* Replace matching Unicode codepoints with some other values.
     * This will use the last matching range found in the list. */
    std::vector<CodepointMap> codepoint_map;

    /* Set a background and foreground color to use for the "screen".
     * For styled formats, this will emit the proper sequences or styles. */
    Maybe<terminal::RGB> background; /* = null */
    Maybe<terminal::RGB> foreground; /* = null */

    /* If set, then styled formats in `emit` will use this palette to
     * emit colors directly as RGB. If this is null, styled formats will
     * still work but will use deferred palette styling (e.g. CSS variables
     * for HTML or the actual palette indexes for VT). */
    const terminal::Palette *palette; /* = null */

    explicit Options(Format e = Format::plain, bool unwrap_ = false, bool trim_ = true)
        : emit(e), unwrap(unwrap_), trim(trim_), codepoint_map(), background(), foreground(), palette(nullptr) {}

    static Options plain() { return Options(Format::plain); }
    static Options vt() { return Options(Format::vt); }
    static Options html() { return Options(Format::html); }
};

/* Maps byte positions in formatted output to PageList pins.
 *
 * Used by formatters that operate on PageLists to track the source position
 * of each byte written. The caller is responsible for freeing the map.
 *
 * The mapping is stored in two parts: a per-byte x/y coordinate (8
 * bytes per output byte, half the size of a Pin) and a tiny table of
 * page nodes covering byte ranges (there are only ever a handful of
 * pages). This also lets page formatters write coordinates directly
 * into the map without a separate coordinate-to-pin conversion pass. */
struct PinMap {
    /* The type of the page node referenced by pins. */
    typedef PageList::Node *Node;

    /* A page node covering output bytes starting at `offset`
     * (inclusive) until the next entry's offset (or the end of the
     * output). */
    struct NodeRun {
        size_t offset;
        Node node;
    };

    struct Map {
        /* The x/y coordinate within its page for every output byte. */
        std::vector<Coordinate> points;

        /* The page node for ranges of output bytes, ordered by offset. */
        std::vector<NodeRun> nodes;

        void clearRetainingCapacity() {
            points.clear();
            nodes.clear();
        }

        /* The total number of bytes mapped. */
        size_t count() const { return points.size(); }

        /* Set the page node for all bytes appended from here on,
         * until the next call. No-op if the node is unchanged. */
        void setNode(Node node) {
            if (!nodes.empty() && nodes.back().node == node) return;
            NodeRun r = {points.size(), node};
            nodes.push_back(r);
        }

        /* Append `n` bytes that map to `pin`. */
        void append(const Pin &pin, size_t n) {
            if (n == 0) return;
            setNode(pin.node);
            points.insert(points.end(), n, Coordinate(pin.x, pin.y));
        }

        /* Returns the pin that the byte at the given offset maps to,
         * or null if the offset is out of range. */
        Maybe<Pin> get(size_t offset) const {
            if (offset >= points.size()) return Maybe<Pin>::none();
            const Coordinate coord = points[offset];
            const Node node = findNode(nodes, offset);
            if (!node) return Maybe<Pin>::none();
            return Pin(node, (size::CellCountInt)coord.y, coord.x);
        }

        /* Returns the last pin in the map, if any. */
        Maybe<Pin> getLastOrNull() const {
            const size_t len = points.size();
            if (len == 0) return Maybe<Pin>::none();
            return get(len - 1);
        }
    };

    Map *map;

    /* Binary search for the node covering `offset` in a slice of node
     * runs sorted by offset. Returns null only if the slice is empty
     * or the offset precedes the first run. */
    static Node findNode(const std::vector<NodeRun> &runs, size_t offset) {
        if (runs.empty() || offset < runs[0].offset) return nullptr;
        size_t lo = 0;
        size_t hi = runs.size();
        while (lo + 1 < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (runs[mid].offset <= offset)
                lo = mid;
            else
                hi = mid;
        }
        return runs[lo].node;
    }
};

namespace detail {
inline void writeUtf8(std::string *w, uint32_t cp) {
    /* Writer.printUnicodeCodepoint: invalid codepoints become U+FFFD. */
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        w->push_back((char)cp);
    } else if (cp < 0x800) {
        w->push_back((char)(0xC0 | (cp >> 6)));
        w->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        w->push_back((char)(0xE0 | (cp >> 12)));
        w->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        w->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        w->push_back((char)(0xF0 | (cp >> 18)));
        w->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        w->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        w->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

inline void hex2(std::string *w, uint8_t v) {
    static const char digits[] = "0123456789abcdef";
    w->push_back(digits[v >> 4]);
    w->push_back(digits[v & 0xF]);
}

inline void printDec(std::string *w, size_t v) { *w += std::to_string(v); }
} /* namespace detail */

/* Page formatter.
 *
 * For styled formatting such as VT, this will emit references for palette
 * colors. If you want to capture the palette as-is at the type of formatting,
 * you'll have to emit the sequences for setting up the palette prior to
 * this formatting. (TODO: A function to do this) */
struct PageFormatter {
    /* The page to format. */
    const Page *page;

    /* The common options */
    const Options *opts;

    /* Start and end points within the page to format. If end x is not given
     * then it will be the full width. If end y is not given then it will be
     * the full height.
     *
     * The start and end are both inclusive, so equal values will still
     * return a non-empty result (i.e. a single cell or row).
     *
     * The start x is considered the X in the first row and end X is
     * X in the final row. This isn't a rectangle selection by default.
     *
     * If start X falls on the second column of a wide character, then
     * the entire character will be included (as if you specified the
     * previous column). */
    size::CellCountInt start_x;
    size::CellCountInt start_y;
    Maybe<size::CellCountInt> end_x;
    Maybe<size::CellCountInt> end_y;

    /* If true, the start x/y and end x/y define a rectangle selection.
     * In this case, the boundaries will apply to every row, not just
     * the first and last. */
    bool rectangle;

    /* See point_map. */
    struct PointMap {
        std::vector<Coordinate> *map;

        /* The index in `map` at which this formatter's output begins.
         * Entries before this index belong to a caller (e.g. previous
         * pages of a PageListFormatter) and aren't inspected. This
         * exists so that callers can share one list across pages. */
        size_t base; /* = 0 */
    };

    /* If non-null, then `map` will contain the x/y coordinate of every
     * byte written to the writer offset by the byte index. It is the
     * caller's responsibility to free the map.
     *
     * The x/y coordinate will be the coordinates within the page.
     *
     * Warning: there is a significant performance hit to track this */
    Maybe<PointMap> point_map;

    /* Trailing state. This is used to ensure that rows wrapped across
     * multiple pages are unwrapped properly, as well as other accounting
     * we may do in the future. */
    struct TrailingState {
        size_t rows; /* = 0 */
        size_t cells; /* = 0 */

        static TrailingState empty() {
            TrailingState s = {0, 0};
            return s;
        }
    };

    /* The previous trailing state from the prior page. If you're iterating
     * over multiple pages this helps ensure that unwrapping and other
     * accounting works properly. */
    Maybe<TrailingState> trailing_state;

    /* Initializes a page formatter. Other options can be set directly on the
     * struct after initialization and before calling `format()`. */
    static PageFormatter init(const Page *page, const Options *opts) {
        PageFormatter f;
        f.page = page;
        f.opts = opts;
        f.start_x = 0;
        f.start_y = 0;
        f.end_x = Maybe<size::CellCountInt>();
        f.end_y = Maybe<size::CellCountInt>();
        f.rectangle = false;
        f.point_map = Maybe<PointMap>();
        f.trailing_state = Maybe<TrailingState>();
        return f;
    }

    void format(std::string *writer) const { (void)formatWithState(writer); }

    static void appendN(std::vector<Coordinate> *m, Coordinate c, size_t n) { m->insert(m->end(), n, c); }

    TrailingState formatWithState(std::string *writer) const {
        const Format emit = opts->emit;
        size_t blank_rows = 0;
        size_t blank_cells = 0;

        /* Continue our prior trailing state if we have it, but only if we're
         * starting from the beginning (start_y and start_x are both 0).
         * If a non-zero start position is specified, ignore trailing state. */
        if (trailing_state.has) {
            if (start_y == 0 && start_x == 0) {
                blank_rows = trailing_state.value.rows;
                blank_cells = trailing_state.value.cells;
            }
        }
        TrailingState early = {blank_rows, blank_cells};

        /* Setup our starting column and perform some validation for overflows.
         * Note: start_x only applies to the first row, end_x only applies to the last row. */
        const size::CellCountInt sx = start_x;
        if (sx >= page->size.cols) return early;
        const size::CellCountInt end_x_unclamped = end_x.orelse((size::CellCountInt)(page->size.cols - 1));
        size::CellCountInt ex =
            end_x_unclamped < page->size.cols - 1 ? end_x_unclamped : (size::CellCountInt)(page->size.cols - 1);

        /* Setup our starting row and perform some validation for overflows. */
        const size::CellCountInt sy = start_y;
        if (sy >= page->size.rows) return early;
        const size::CellCountInt end_y_unclamped = end_y.orelse((size::CellCountInt)(page->size.rows - 1));
        if (sy > end_y_unclamped) return early;
        size::CellCountInt ey =
            end_y_unclamped < page->size.rows - 1 ? end_y_unclamped : (size::CellCountInt)(page->size.rows - 1);

        /* Edge case: if our end x/y falls on a spacer head AND we're unwrapping,
         * then we move the x/y to the start of the next row (if available). */
        if (opts->unwrap && !rectangle) {
            const Row *final_row = page->getRow(ey);
            const Cell *cells = page->getCells(final_row);
            if (cells[ex].wide() == Cell::Wide::spacer_head) {
                /* Move to next row if available
                 *
                 * TODO: if unavailable, we should add to our trailing state
                 *
                 * so the pagelist formatter can be aware and maybe add
                 * another page */
                if (ey < page->size.rows - 1) {
                    ey += 1;
                    ex = 0;
                }
            }
        }

        /* If we only have a single row, validate that start_x <= end_x */
        if (sy == ey && sx > ex) return early;

        /* Wrap HTML output in monospace font styling */
        switch (emit) {
        case Format::plain: break;

        case Format::html: {
            /* Setup our div. We use a buffer here that should always
             * fit the stuff we need, in order to make counting bytes easier. */
            std::string header;

            /* Monospace and whitespace preserving */
            header += "<div style=\"font-family: monospace; white-space: pre;";

            /* Background/foreground colors */
            if (opts->background.has) {
                header += "background-color: #";
                detail::hex2(&header, opts->background.value.r);
                detail::hex2(&header, opts->background.value.g);
                detail::hex2(&header, opts->background.value.b);
                header += ";";
            }
            if (opts->foreground.has) {
                header += "color: #";
                detail::hex2(&header, opts->foreground.value.r);
                detail::hex2(&header, opts->foreground.value.g);
                detail::hex2(&header, opts->foreground.value.b);
                header += ";";
            }

            header += "\">";

            *writer += header;
            if (point_map.has) appendN(point_map.value.map, Coordinate(0, 0), header.size());
            break;
        }

        case Format::vt: {
            /* OSC 10 sets foreground color, OSC 11 sets background color */
            std::string header;
            if (opts->foreground.has) {
                header += "\x1b]10;rgb:";
                detail::hex2(&header, opts->foreground.value.r);
                header += "/";
                detail::hex2(&header, opts->foreground.value.g);
                header += "/";
                detail::hex2(&header, opts->foreground.value.b);
                header += "\x1b\\";
            }
            if (opts->background.has) {
                header += "\x1b]11;rgb:";
                detail::hex2(&header, opts->background.value.r);
                header += "/";
                detail::hex2(&header, opts->background.value.g);
                header += "/";
                detail::hex2(&header, opts->background.value.b);
                header += "\x1b\\";
            }

            *writer += header;
            if (point_map.has) appendN(point_map.value.map, Coordinate(0, 0), header.size());
            break;
        }
        }

        /* Our style for non-plain formats. Alongside the style itself we
         * track the page-local interned style id it corresponds to (styles
         * are interned per-page so id equality implies style equality).
         * The id is only a fast-path hint: it is set to `invalid_style_id`
         * whenever the current style didn't come from an interned id
         * (e.g. bg-color-only cells which synthesize styles). */
        const uint32_t invalid_style_id = 0xFFFFFFFFu;
        Style style;
        uint32_t style_id = 0;

        /* Track hyperlink state for HTML output. We need to close </a> tags
         * when the hyperlink changes or ends. */
        Maybe<hyperlink::Id> current_hyperlink_id;

        for (size_t y_usize = sy; y_usize < (size_t)ey + 1; y_usize++) {
            const size::CellCountInt y = (size::CellCountInt)y_usize;
            const Row *row = page->getRow(y);
            const Cell *cells = page->getCells(row);

            /* Determine the x range for this row
             * - First row: start_x to end of row (or end_x if single row)
             * - Last row: start of row to end_x
             * - Middle rows: full width */
            /* The end is always straightforward */
            const size::CellCountInt row_end_x =
                (rectangle || y == ey) ? (size::CellCountInt)(ex + 1) : page->size.cols;

            /* The first we have to check if our start X falls on the
             * tail of a wide character. */
            size::CellCountInt row_start_x = 0;
            if (sx > 0 && (rectangle || y == sy)) {
                bool skip_row = false;
                switch (cells[sx].wide()) {
                /* Include the prior cell to get the full wide char */
                case Cell::Wide::spacer_tail: row_start_x = (size::CellCountInt)(sx - 1); break;

                /* If we're a spacer head on our first row then we
                 * skip this whole row. */
                case Cell::Wide::spacer_head: skip_row = true; break;

                default: row_start_x = sx; break;
                }
                if (skip_row) continue;
            }

            const Cell *cells_subset = cells + row_start_x;
            const size_t subset_len = (size_t)row_end_x - row_start_x;

            /* If this row is blank, accumulate to avoid a bunch of extra
             * work later. If it isn't blank, make sure we dump all our
             * blanks. */
            if (!Cell::hasTextAny(cells_subset, subset_len)) {
                blank_rows += 1;
                continue;
            }

            if (blank_rows > 0) {
                /* Reset style before emitting newlines to prevent background
                 * colors from bleeding into the next line's leading cells. */
                if (!style.default_()) {
                    formatStyleClose(emit, writer);
                    style = Style();
                    style_id = 0;
                }

                const char *sequence;
                switch (emit) {
                /* Plaintext just uses standard newlines because newlines
                 * on their own usually move the cursor back in anywhere
                 * you type plaintext. */
                case Format::plain: sequence = "\n"; break;

                /* VT uses \r\n because in a raw pty, \n alone doesn't
                 * guarantee moving the cursor back to column 0. \r
                 * makes it work for sure. */
                case Format::vt: sequence = "\r\n"; break;

                /* HTML uses just \n because HTML rendering will move
                 * the cursor back. */
                default: sequence = "\n"; break;
                }
                const size_t seq_len = strlen(sequence);

                for (size_t i = 0; i < blank_rows; i++) *writer += sequence;

                /* \r and \n map to the row that ends with this newline.
                 * If we're continuing (trailing state) then this will be
                 * in a prior page, so we just map to the first row of this
                 * page. */
                if (point_map.has) {
                    std::vector<Coordinate> *map = point_map.value.map;
                    const Coordinate start =
                        map->size() > point_map.value.base ? map->back() : Coordinate(0, 0);

                    /* The first one inherits the x value. */
                    appendN(map, Coordinate(start.x, start.y), seq_len);

                    /* All others have x = 0 since they reference their prior
                     * blank line. */
                    for (size_t y_offset = 1; y_offset < blank_rows; y_offset++) {
                        appendN(map, Coordinate(0, (uint32_t)(start.y + y_offset)), seq_len);
                    }
                }

                blank_rows = 0;
            }

            /* If we're not wrapped, we always add a newline so after
             * the row is printed we can add a newline. */
            if (!row->wrap() || !opts->unwrap) blank_rows += 1;

            /* If the row doesn't continue a wrap then we need to reset
             * our blank cell count. */
            if (!row->wrap_continuation() || !opts->unwrap) blank_cells = 0;

            /* Go through each cell and print it */
            for (size_t cell_i = 0; cell_i < subset_len; cell_i++) {
                const Cell *cell = &cells_subset[cell_i];
                const size::CellCountInt x = (size::CellCountInt)(row_start_x + cell_i);

                /* Skip spacers. These happen naturally when wide characters
                 * are printed again on the screen (for well-behaved terminals!) */
                if (cell->wide() == Cell::Wide::spacer_head || cell->wide() == Cell::Wide::spacer_tail) continue;

                /* If we have a zero value, then we accumulate a counter. We
                 * only want to turn zero values into spaces if we have a non-zero
                 * char sometime later. */
                {
                    bool is_blank = true;
                    /* If we're emitting styled output (not plaintext) and
                     * the cell has some kind of styling or is not empty
                     * then this isn't blank. */
                    if (formatStyled(emit) && (!cell->isEmpty() || cell->hasStyling())) is_blank = false;

                    if (is_blank) {
                        /* Cells with no text are blank */
                        if (!cell->hasText()) {
                            blank_cells += 1;
                            continue;
                        }

                        /* Trailing spaces are blank. We know it is trailing
                         * because if we get a non-empty cell later we'll
                         * fill the blanks. */
                        if (cell->codepoint() == ' ' && opts->trim) {
                            blank_cells += 1;
                            continue;
                        }
                    }
                }

                /* This cell is not blank. If we have accumulated blank cells
                 * then we want to emit them now. */
                if (blank_cells > 0) {
                    writer->append(blank_cells, ' ');

                    if (point_map.has) appendBlankPoints(point_map.value, blank_cells, x, y);

                    blank_cells = 0;
                }

                /* style: */
                do {
                    /* If we aren't emitting styled output then we don't
                     * have to worry about styles. */
                    if (!formatStyled(emit)) break;

                    /* Fast path: styles are interned per-page, so if this
                     * cell's style id matches the id of our current style
                     * then the style is unchanged. */
                    const uint32_t cell_style_id =
                        (cell->content_tag() == Cell::ContentTag::codepoint ||
                         cell->content_tag() == Cell::ContentTag::codepoint_grapheme)
                            ? cell->style_id()
                            : invalid_style_id;
                    if (cell_style_id == style_id && cell_style_id != invalid_style_id) break;

                    /* Get our cell style. */
                    const Style cell_style = cellStyle(cell);

                    /* If the style hasn't changed, don't bloat output.
                     * When both ids are interned (and thus different, since
                     * equal ids broke out above), interning guarantees the
                     * styles differ so we can skip the comparison entirely. */
                    if (cell_style_id == invalid_style_id || style_id == invalid_style_id) {
                        if (cell_style.eql(style)) {
                            style_id = cell_style_id;
                            break;
                        }
                    }

                    /* If we had a previous style, we need to close it,
                     * because we've confirmed we have some new style
                     * (which is maybe default). */
                    if (!style.default_()) {
                        switch (emit) {
                        case Format::html: formatStyleClose(emit, writer); break;

                        /* For VT, we only close if we're switching to a default
                         * style because any non-default style will emit
                         * a \x1b[0m as the start of a VT coloring sequence. */
                        case Format::vt:
                            if (cell_style.default_()) formatStyleClose(emit, writer);
                            break;

                        default: assert(false); break;
                        }
                    }

                    /* At this point, we can copy our style over */
                    style = cell_style;
                    style_id = cell_style_id;

                    /* If we're just the default style now, we're done. */
                    if (cell_style.default_()) break;

                    /* New style, emit it. */
                    const size_t before = writer->size();
                    formatStyleOpen(emit, writer, &style);

                    /* If we have a point map, we map the style to
                     * this cell. */
                    if (point_map.has) appendN(point_map.value.map, Coordinate(x, y), writer->size() - before);
                } while (false);

                /* Hyperlink state */
                do {
                    /* We currently only emit hyperlinks for HTML. In the
                     * future we can support emitting OSC 8 hyperlinks for
                     * VT output as well. */
                    if (emit != Format::html) break;

                    /* Get the hyperlink ID. This ID is our internal ID,
                     * not necessarily the OSC8 ID. */
                    Maybe<hyperlink::Id> link_id_;
                    if (cell->hyperlink()) {
                        hyperlink::Id lid;
                        if (page->lookupHyperlink(cell, &lid)) link_id_ = lid;
                    }

                    /* If our hyperlink IDs match (even null) then we have
                     * identical hyperlink state and we do nothing. */
                    if (current_hyperlink_id.has == link_id_.has &&
                        (!link_id_.has || current_hyperlink_id.value == link_id_.value))
                        break;

                    /* If our prior hyperlink ID was non-null, we need to
                     * close it because the ID has changed. */
                    if (current_hyperlink_id.has) {
                        formatHyperlinkClose(emit, writer);
                        current_hyperlink_id = Maybe<hyperlink::Id>::none();
                    }

                    /* Set our current hyperlink ID */
                    if (!link_id_.has) break;
                    const hyperlink::Id link_id = link_id_.value;
                    current_hyperlink_id = link_id;

                    /* Emit the opening hyperlink tag */
                    const hyperlink::PageEntry *link = page->hyperlink_set.get((const void *)page->memory, link_id);
                    const uint8_t *uri = link->uri.slice((const void *)page->memory);
                    const size_t before = writer->size();
                    formatHyperlinkOpen(emit, writer, uri, link->uri.len);

                    /* If we have a point map, we map the hyperlink to
                     * this cell. */
                    if (point_map.has) appendN(point_map.value.map, Coordinate(x, y), writer->size() - before);
                } while (false);

                switch (cell->content_tag()) {
                /* We combine codepoint and graphemes because both have
                 * shared style handling. We use comptime to dup it. */
                case Cell::ContentTag::codepoint:
                case Cell::ContentTag::codepoint_grapheme: {
                    const size_t before = writer->size();
                    writeCell(emit, writer, cell);

                    /* If we have a point map, all codepoints map to this
                     * cell. */
                    if (point_map.has) appendN(point_map.value.map, Coordinate(x, y), writer->size() - before);
                    break;
                }

                /* Cells with only background color (no text). Emit a space
                 * with the appropriate background color SGR sequence. */
                default:
                    writer->push_back(' ');
                    if (point_map.has) point_map.value.map->push_back(Coordinate(x, y));
                    break;
                }
            }
        }

        /* If the style is non-default, we need to close our style tag. */
        if (!style.default_()) formatStyleClose(emit, writer);

        /* Close any open hyperlink for HTML output */
        if (current_hyperlink_id.has) formatHyperlinkClose(emit, writer);

        /* Close the monospace wrapper for HTML output */
        if (emit == Format::html) {
            const char *closing = "</div>";
            *writer += closing;
            if (point_map.has) {
                std::vector<Coordinate> *m = point_map.value.map;
                const Coordinate last = m->back();
                appendN(m, last, strlen(closing));
            }
            /* Closing the div creates a newline in the output
             * so make sure to create one less newline */
            if (blank_rows >= 1) blank_rows -= 1;
        }

        TrailingState out = {blank_rows, blank_cells};
        return out;
    }

    /* Append the point map entries for a run of `count` blank cells
     * that are materialized as spaces just before the cell at (x, y).
     * Blank cells can span multiple rows if they carry over from wrap
     * continuation, so this walks backwards from (x, y). */
    void appendBlankPoints(const PointMap &map, size_t count, size::CellCountInt x, size::CellCountInt y) const {
        size_t remaining = count;
        size::CellCountInt blank_x = x;
        size::CellCountInt blank_y = y;
        for (; remaining > 0; remaining -= 1) {
            if (blank_x > 0) {
                /* We have space in this row */
                blank_x -= 1;
            } else if (blank_y > 0) {
                /* Wrap to previous row */
                blank_y -= 1;
                blank_x = (size::CellCountInt)(page->size.cols - 1);
            } else {
                /* Can't go back further, just use (0, 0) */
                blank_x = 0;
                blank_y = 0;
            }

            map.map->push_back(Coordinate(blank_x, blank_y));
        }
    }

    void writeCell(Format emit, std::string *writer, const Cell *cell) const {
        /* Blank cells get an empty space that isn't replaced by anything
         * because it isn't really a space. We do this so that formatting
         * is preserved if we're emitting styles. */
        if (!cell->hasText()) {
            writer->push_back(' ');
            return;
        }

        writeCodepointWithReplacement(emit, writer, cell->contentCodepoint());
        if (cell->content_tag() == Cell::ContentTag::codepoint_grapheme) {
            size_t len = 0;
            const uint32_t *cps = page->lookupGrapheme(cell, &len);
            for (size_t i = 0; i < len; i++) writeCodepointWithReplacement(emit, writer, cps[i]);
        }
    }

    void writeCodepointWithReplacement(Format emit, std::string *writer, uint32_t codepoint) const {
        /* Search for our replacement */
        const CodepointMap::Replacement *r = nullptr;
        const std::vector<CodepointMap> &items = opts->codepoint_map;
        for (size_t forward_i = 0; forward_i < items.size(); forward_i++) {
            const size_t i = items.size() - forward_i - 1;
            if (items[i].range[0] <= codepoint && codepoint <= items[i].range[1]) {
                r = &items[i].replacement;
                break;
            }
        }

        /* If no replacement, write it directly. */
        if (!r) {
            writeCodepoint(emit, writer, codepoint);
            return;
        }

        switch (r->tag) {
        case CodepointMap::Replacement::Tag::codepoint: writeCodepoint(emit, writer, r->codepoint); break;

        case CodepointMap::Replacement::Tag::string: {
            const std::string &s = r->string;
            size_t i = 0;
            while (i < s.size()) {
                const uint8_t c0 = (uint8_t)s[i];
                uint32_t cp;
                size_t n;
                if (c0 < 0x80) {
                    cp = c0;
                    n = 1;
                } else if ((c0 >> 5) == 6) {
                    cp = c0 & 0x1F;
                    n = 2;
                } else if ((c0 >> 4) == 14) {
                    cp = c0 & 0x0F;
                    n = 3;
                } else {
                    cp = c0 & 0x07;
                    n = 4;
                }
                for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
                writeCodepoint(emit, writer, cp);
                i += n;
            }
            break;
        }
        }
    }

    void writeCodepoint(Format emit, std::string *writer, uint32_t codepoint) const {
        switch (emit) {
        case Format::plain:
        case Format::vt: detail::writeUtf8(writer, codepoint); break;
        case Format::html:
            switch (codepoint) {
            case '<': *writer += "&lt;"; break;
            case '>': *writer += "&gt;"; break;
            case '&': *writer += "&amp;"; break;
            case '"': *writer += "&quot;"; break;
            case '\'': *writer += "&#39;"; break;
            default:
                /* For HTML, emit ASCII (< 0x80) directly, but encode
                 * all non-ASCII as numeric entities to avoid encoding
                 * detection issues (fixes #9426). We can't set the
                 * meta tag because we emit partial HTML so this ensures
                 * proper unicode handling. */
                if (codepoint < 0x80) {
                    writer->push_back((char)codepoint);
                } else {
                    *writer += "&#";
                    detail::printDec(writer, codepoint);
                    *writer += ";";
                }
                break;
            }
            break;
        }
    }

    /* Returns the style for the given cell. If there is no styling this
     * will return the default style. */
    Style cellStyle(const Cell *cell) const {
        switch (cell->content_tag()) {
        case Cell::ContentTag::codepoint:
        case Cell::ContentTag::codepoint_grapheme:
            if (!cell->hasStyling()) return Style();
            return *page->styles.get((const void *)page->memory, cell->style_id());

        case Cell::ContentTag::bg_color_palette: {
            Style s;
            s.bg_color = Style::Color::makePalette(cell->contentColorPalette());
            return s;
        }

        default: {
            Style s;
            const Cell::RGB c = cell->contentColorRgb();
            s.bg_color = Style::Color::makeRgb(style::RGB(c.r, c.g, c.b));
            return s;
        }
        }
    }

    /* Write a string with HTML escaping. Used for escaping href attributes
     * and other HTML attribute values. */
    void formatStyleOpen(Format emit, std::string *writer, const Style *style) const {
        switch (emit) {
        case Format::plain: assert(false); break;

        case Format::vt: {
            Style::VTFormatter f = style->formatterVt();
            f.palette = opts->palette;
            f.format(writer);
            break;
        }

        /* We use `display: inline` so that the div doesn't impact
         * layout since we're primarily using it as a CSS wrapper. */
        case Format::html: {
            Style::HtmlFormatter f = style->formatterHtml();
            f.palette = opts->palette;
            *writer += "<div style=\"display: inline;";
            f.format(writer);
            *writer += "\">";
            break;
        }
        }
    }

    void formatStyleClose(Format emit, std::string *writer) const {
        const char *str;
        switch (emit) {
        case Format::plain: return;
        case Format::vt: str = "\x1b[0m"; break;
        default: str = "</div>"; break;
        }

        *writer += str;
        if (point_map.has) {
            std::vector<Coordinate> *m = point_map.value.map;
            assert(!m->empty());
            const Coordinate last = m->back();
            appendN(m, last, strlen(str));
        }
    }

    void formatHyperlinkOpen(Format emit, std::string *writer, const uint8_t *uri, size_t uri_len) const {
        switch (emit) {
        case Format::plain:
        case Format::vt: assert(false); break;

        /* layout since we're primarily using it as a CSS wrapper. */
        case Format::html:
            *writer += "<a href=\"";
            for (size_t i = 0; i < uri_len; i++) writeCodepoint(emit, writer, uri[i]);
            *writer += "\">";
            break;
        }
    }

    void formatHyperlinkClose(Format emit, std::string *writer) const {
        if (emit != Format::html) return;
        const char *str = "</a>";

        *writer += str;
        if (point_map.has) {
            std::vector<Coordinate> *m = point_map.value.map;
            assert(!m->empty());
            const Coordinate last = m->back();
            appendN(m, last, strlen(str));
        }
    }
};

/* PageList formatter formats multiple pages as represented by a PageList. */
struct PageListFormatter {
    /* The pagelist to format. */
    const PageList *list;

    /* The common options */
    const Options *opts;

    /* The bounds of the PageList to format. The top left and bottom right
     * MUST be ordered properly. */
    Maybe<Pin> top_left;
    Maybe<Pin> bottom_right;

    /* If true, the boundaries define a rectangle selection where start_x
     * and end_x apply to every row, not just the first and last. */
    bool rectangle;

    /* If non-null, then `map` will contain the Pin of every byte
     * byte written to the writer offset by the byte index. It is the
     * caller's responsibility to free the map.
     *
     * Warning: there is a significant performance hit to track this */
    Maybe<PinMap> pin_map;

    static PageListFormatter init(const PageList *list, const Options *opts) {
        PageListFormatter f;
        f.list = list;
        f.opts = opts;
        f.top_left = Maybe<Pin>();
        f.bottom_right = Maybe<Pin>();
        f.rectangle = false;
        f.pin_map = Maybe<PinMap>();
        return f;
    }

    void format(std::string *writer) const {
        const Pin tl = top_left.has ? top_left.value : list->getTopLeft(point::Tag::screen);
        const Pin br = bottom_right.has ? bottom_right.value : list->getBottomRight(point::Tag::screen).value;

        Maybe<PageFormatter::TrailingState> page_state;
        PageList::PageIterator iter = tl.pageIterator(PageList::Direction::right_down, br);
        PageList::Chunk chunk;
        while (iter.next(&chunk)) {
            assert(chunk.start < chunk.end);
            assert(chunk.end > 0);

            PageFormatter formatter = PageFormatter::init(chunk.node->page(), opts);
            formatter.start_y = chunk.start;
            formatter.end_y = (size::CellCountInt)(chunk.end - 1);
            formatter.trailing_state = page_state;
            formatter.rectangle = rectangle;

            /* For rectangle selection, apply start_x and end_x to all chunks */
            if (rectangle) {
                formatter.start_x = tl.x;
                formatter.end_x = br.x;
            } else {
                /* Otherwise only on the first/last, respectively. */
                if (chunk.node == tl.node) formatter.start_x = tl.x;
                if (chunk.node == br.node) formatter.end_x = br.x;
            }

            /* If we're tracking pins, the page formatter writes its
             * per-byte coordinates directly into our map's point list
             * and we record which page node covers those bytes. */
            if (pin_map.has) {
                PinMap::Map *m = pin_map.value.map;
                m->setNode(chunk.node);
                PageFormatter::PointMap pm = {&m->points, m->points.size()};
                formatter.point_map = pm;
            }

            page_state = formatter.formatWithState(writer);
        }
    }
};

struct ScreenFormatter {
    /* The screen to format. */
    const Screen *screen;

    /* The common options */
    Options opts;

    /* The content to include. */
    struct Content {
        enum class Tag {
            /* Emit no content, only terminal state such as modes, palette, etc.
             * via extra. */
            none,

            /* Emit the content specified by the selection. Null for all.
             * The selection is inclusive on both ends. */
            selection,
        } tag;
        Maybe<Selection> selection;
    } content;

    /* Extra stuff to emit, such as cursor, style, hyperlinks, etc.
     * This information is ONLY emitted when the format is "vt". */
    struct Extra {
        /* Emit cursor position using CUP (CSI H). */
        bool cursor;

        /* Emit current SGR style state based on the cursor's active style_id.
         * This reconstructs the SGR attributes (bold, italic, colors, etc.) at
         * the cursor position. */
        bool style;

        /* Emit current hyperlink state using OSC 8 sequences.
         * This sets the active hyperlink based on cursor.hyperlink_id. */
        bool hyperlink;

        /* Emit character protection mode using DECSCA. */
        bool protection;

        /* Emit Kitty keyboard protocol state using CSI > u and CSI = sequences. */
        bool kitty_keyboard;

        /* Emit character set designations and invocations.
         * This includes G0-G3 designations (ESC ( ) * +) and GL/GR invocations. */
        bool charsets;

        /* Emit nothing. */
        static Extra none() {
            Extra e = {false, false, false, false, false, false};
            return e;
        }

        /* Emit style-relevant information only. */
        static Extra styles() {
            Extra e = {false, true, true, false, false, false};
            return e;
        }

        /* Emit everything. This reconstructs the screen state as closely
         * as possible. */
        static Extra all() {
            Extra e = {true, true, true, true, true, true};
            return e;
        }

        bool isSet() const { return cursor || style || hyperlink || protection || kitty_keyboard || charsets; }
    } extra;

    /* If non-null, then `map` will contain the Pin of every byte
     * byte written to the writer offset by the byte index. It is the
     * caller's responsibility to free the map.
     *
     * Note that some emitted bytes may not correspond to any Pin, such as
     * the extra data around screen state. For these, we'll map it to the
     * most previous pin so there is some continuity but its an arbitrary
     * choice.
     *
     * Warning: there is a significant performance hit to track this */
    Maybe<PinMap> pin_map;

    static ScreenFormatter init(const Screen *screen, const Options &opts) {
        ScreenFormatter f;
        f.screen = screen;
        f.opts = opts;
        f.content.tag = Content::Tag::selection;
        f.content.selection = Maybe<Selection>();
        f.extra = Extra::none();
        f.pin_map = Maybe<PinMap>();
        return f;
    }

    void format(std::string *writer) const {
        switch (content.tag) {
        case Content::Tag::none: break;

        case Content::Tag::selection: {
            /* Emit our pagelist contents according to our selection. */
            PageListFormatter list_formatter = PageListFormatter::init(&screen->pages, &opts);
            list_formatter.pin_map = pin_map;
            if (content.selection.has) {
                const Selection &sel = content.selection.value;
                list_formatter.top_left = sel.topLeft(screen);
                list_formatter.bottom_right = sel.bottomRight(screen);
                list_formatter.rectangle = sel.rectangle;
            }
            list_formatter.format(writer);
            break;
        }
        }

        /* Emit extra screen state after content if we care. The state has
         * to be emitted after since some state such as cursor position and
         * style are impacted by content rendering. */
        switch (opts.emit) {
        case Format::plain: return;
        case Format::vt:
            if (!extra.isSet()) return;
            break;

        /* HTML doesn't preserve any screen state because it has
         * nothing to do with rendering. */
        case Format::html: return;
        }

        const size_t extras_start = writer->size();

        /* Emit cursor position before the other extras because restoring a
         * pending wrap requires reprinting the cell at the right edge. That
         * print uses and changes active screen state, so the requested style,
         * hyperlink, protection, and charset must be restored afterwards. */
        if (extra.cursor) {
            const Screen::Cursor &cursor = screen->cursor;

            /* If we don't have pending wrap, then we can just use CUP. */
            if (!cursor.pending_wrap || cursor.x != screen->pages.cols - 1) {
                *writer += "\x1b[";
                detail::printDec(writer, (size_t)cursor.y + 1);
                *writer += ";";
                detail::printDec(writer, (size_t)cursor.x + 1);
                *writer += "H";
            } else {
                /* Pending wrap, we can't use CUP because it resets pending wrap. */
                const size_t start_x = cursor.page_cell->wide() == Cell::Wide::spacer_tail ? (size_t)cursor.x - 1
                                                                                             : (size_t)cursor.x;

                /* Move cursor to the edge. */
                *writer += "\x1b[";
                detail::printDec(writer, (size_t)cursor.y + 1);
                *writer += ";";
                detail::printDec(writer, start_x + 1);
                *writer += "H";

                /* Reformat the cell which sets the proper pending wrap state. */
                PageFormatter cell_formatter = PageFormatter::init(cursor.page_pin->node->page(), &opts);
                cell_formatter.start_x = cursor.x;
                cell_formatter.end_x = cursor.x;
                cell_formatter.start_y = cursor.page_pin->y;
                cell_formatter.end_y = cursor.page_pin->y;
                cell_formatter.format(writer);
            }
        }

        /* Emit current SGR style state */
        if (extra.style) {
            screen->cursor.style.formatterVt().format(writer);
        }

        /* Emit current hyperlink state using OSC 8 */
        if (extra.hyperlink) {
            const hyperlink::Hyperlink *link = screen->cursor.hyperlink;
            if (link) {
                /* Start hyperlink with uri (and explicit id if present) */
                if (link->id.tag == hyperlink::Hyperlink::Id::Tag::explicit_) {
                    *writer += "\x1b]8;id=";
                    writer->append((const char *)link->id.explicit_ptr, link->id.explicit_len);
                    *writer += ";";
                    writer->append((const char *)link->uri, link->uri_len);
                    *writer += "\x1b\\";
                } else {
                    *writer += "\x1b]8;;";
                    writer->append((const char *)link->uri, link->uri_len);
                    *writer += "\x1b\\";
                }
            }
        }

        /* Emit character protection mode using DECSCA */
        if (extra.protection) {
            if (screen->cursor.protected_) {
                /* DEC protected mode */
                *writer += "\x1b[1\"q";
            }
        }

        /* Emit Kitty keyboard protocol state using CSI = u */
        if (extra.kitty_keyboard) {
            const terminal::kitty::KeyFlags current_flags = screen->kitty_keyboard.current();
            if (current_flags.int_() != terminal::kitty::KeyFlags::disabled().int_()) {
                *writer += "\x1b[=";
                detail::printDec(writer, current_flags.int_());
                *writer += ";1u";
            }
        }

        /* Emit character set designations and invocations */
        if (extra.charsets) {
            typedef terminal::charsets::Slots Slots;
            typedef terminal::charsets::Charset Charset;
            const Screen::CharsetState &charset = screen->charset;

            /* Emit G0-G3 designations */
            const Slots slots[] = {Slots::G0, Slots::G1, Slots::G2, Slots::G3};
            for (Slots slot : slots) {
                const Charset cs = charset.charsets.get(slot);
                if (cs != Charset::utf8) { /* Only emit non-default charsets */
                    char intermediate;
                    switch (slot) {
                    case Slots::G0: intermediate = '('; break;
                    case Slots::G1: intermediate = ')'; break;
                    case Slots::G2: intermediate = '*'; break;
                    default: intermediate = '+'; break;
                    }
                    char final_;
                    switch (cs) {
                    case Charset::ascii: final_ = 'B'; break;
                    case Charset::british: final_ = 'A'; break;
                    case Charset::dec_special: final_ = '0'; break;
                    default: continue;
                    }
                    writer->push_back('\x1b');
                    writer->push_back(intermediate);
                    writer->push_back(final_);
                }
            }

            /* Emit GL invocation if not G0 */
            if (charset.gl != Slots::G0) {
                switch (charset.gl) {
                case Slots::G1: *writer += "\x0e"; break; /* SO - Shift Out */
                case Slots::G2: *writer += "\x1bn"; break; /* LS2 */
                case Slots::G3: *writer += "\x1bo"; break; /* LS3 */
                default: assert(false); break;
                }
            }

            /* Emit GR invocation if not G2 */
            if (charset.gr != Slots::G2) {
                switch (charset.gr) {
                case Slots::G1: *writer += "\x1b~"; break; /* LS1R */
                case Slots::G3: *writer += "\x1b|"; break; /* LS3R */
                default: assert(false); break;             /* GR can't be G0 */
                }
            }
        }

        /* If we have a pin_map, we need to count how many bytes the extras
         * will emit so we can map them all to the same pin. We do this by
         * formatting to a discarding writer with content=none.
         * Wisp: the extras were written above, so count them directly. */
        if (pin_map.has) {
            PinMap::Map *m = pin_map.value.map;

            /* Map all those bytes to the same pin. Use the first page node
             * to ensure the node pointer is always properly initialized. */
            const Maybe<Pin> last = m->getLastOrNull();
            m->append(last.has ? last.value : screen->pages.getTopLeft(point::Tag::screen),
                      writer->size() - extras_start);
        }
    }
};

} /* namespace formatter */

/* ====================================================================== */
/* Screen.zig functions backed by ScreenFormatter                          */

inline std::string Screen::selectionString(const SelectionString &opts) {
    /* Create a formatter and use that to emit our text. */
    formatter::Options fopts(formatter::Format::plain, true, opts.trim);
    formatter::ScreenFormatter f = formatter::ScreenFormatter::init(this, fopts);
    f.content.tag = formatter::ScreenFormatter::Content::Tag::selection;
    f.content.selection = opts.sel;

    /* Emit. */
    std::string aw;
    f.format(&aw);
    return aw;
}

inline void Screen::dumpString(std::string *writer, const DumpString &opts) const {
    /* Create a formatter and use that to emit our text. */
    formatter::Options fopts(formatter::Format::plain, opts.unwrap, false);
    formatter::ScreenFormatter f = formatter::ScreenFormatter::init(this, fopts);

    /* Set up the selection based on the pins */
    const Pin tl = opts.tl;
    const Pin br = opts.br.has ? opts.br.value : pages.getBottomRight(point::Tag::screen).value;

    f.content.tag = formatter::ScreenFormatter::Content::Tag::selection;
    f.content.selection = Selection::init(tl, br, false /* not rectangle */);

    /* Emit */
    f.format(writer);
}

inline std::string Screen::dumpStringAlloc(const point::Point &tl) const {
    std::string builder;
    DumpString o;
    o.tl = pages.getTopLeft(tl.tag);
    const Maybe<Pin> br = pages.getBottomRight(tl.tag);
    assert(br.has); /* Wisp: error.UnknownPoint */
    o.br = br;
    o.unwrap = false;
    dumpString(&builder, o);
    return builder;
}

inline std::string Screen::dumpStringAllocUnwrapped(const point::Point &tl) const {
    std::string builder;
    DumpString o;
    o.tl = pages.getTopLeft(tl.tag);
    const Maybe<Pin> br = pages.getBottomRight(tl.tag);
    assert(br.has); /* Wisp: error.UnknownPoint */
    o.br = br;
    o.unwrap = true;
    dumpString(&builder, o);
    return builder;
}

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_FORMATTER_HPP */
