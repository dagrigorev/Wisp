/* Ported from Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * PARTLY TRANSLITERATED. The Cell and Row bit layouts, Capacity and the page
 * layout were checked against upstream. The operations further down — the
 * grapheme and hyperlink operations, erase, clone, reflow and scrolling —
 * were reimplemented from Ghostty's design and verified against Wisp's own
 * tests, without the upstream source to hand. They are due to be replaced by
 * a transliteration checked against upstream's own tests.
 *
 * Cell and Row, the storage primitives a Page is built from.
 *
 * PARTIAL PORT. Cell, Row, Capacity, the layout and the Page struct's storage
 * layer are here, along with the definitions that close the style and
 * hyperlink cycles and the operations over that storage: grapheme management,
 * erase, clone, reflow and scrolling. Upstream's remaining pieces are not, so
 * the ledger still records page.zig as `wip`.
 *
 * BIT LAYOUT. Both Cell and Row are exactly 64 bits, and the field order
 * matters: it is what lets a row of cells be memcpy'd and a page be relocated
 * wholesale. Zig packs a packed struct from the least significant bit upward,
 * so the field order below matches the declaration order upstream.
 *
 *   Cell                                Row
 *     0..1   content_tag                  0..31  cells (Offset)
 *     2..25  content (union)             32      wrap
 *    26..41  style_id                    33      wrap_continuation
 *    42..43  wide                        34      grapheme
 *    44      protected                   35      styled
 *    45      hyperlink                   36      hyperlink
 *    46..47  semantic_content            37..38  semantic_prompt
 *    48..63  padding                     39      kitty_virtual_placeholder
 *                                        40      dirty
 *                                        41..63  padding
 *
 * This is written as explicit shifts over a uint64_t rather than as C++
 * bitfields. Bitfield allocation order is implementation-defined, so a
 * bitfield version would be a layout that happens to work on one compiler
 * rather than the layout being reproduced.
 */

#pragma once
#ifndef WISP_TERMINAL_PAGE_HPP
#define WISP_TERMINAL_PAGE_HPP

#include <stddef.h>
#include <stdint.h>

#include "size.hpp"
#include "color.hpp"
#include "style.hpp"
#include "bitmap_allocator.hpp"
#include "hash_map.hpp"
#include "hyperlink.hpp"

namespace wisp {
namespace terminal {

/* ─── cell enums ─────────────────────────────────────────────────────────── */

/* Selects which member of a cell's content union is active, and affects some
 * behavior besides. */
enum class ContentTag : uint8_t {
    /* A single codepoint, possibly zero for an empty cell. */
    codepoint = 0,

    /* A codepoint that begins a multi-codepoint grapheme cluster. The
     * codepoint member is still active, but more codepoints live in the
     * page's grapheme data. */
    codepoint_grapheme = 1,

    /* No text, only a background color. Keeping these out of the style map
     * saves both a style slot and a lookup, which matters because large runs
     * of background-only cells are common. */
    bg_color_palette = 2,
    bg_color_rgb = 3,
};

/* A grid cell is one or two columns wide, and a wide character is always
 * followed by a spacer. This encodes both the width and the spacer role. */
enum class Wide : uint8_t {
    narrow = 0,
    wide = 1,

    /* Follows a wide character; not rendered. */
    spacer_tail = 2,

    /* Sits at the end of a soft-wrapped line to show that a wide character
     * continues on the next one. */
    spacer_head = 3,
};

/* Semantic type of a cell's content, from OSC 133. */
enum class SemanticContent : uint8_t {
    output = 0,
    input = 1,
    prompt = 2,
};

/* Whether a row holds prompt cells, and which kind. */
enum class SemanticPrompt : uint8_t {
    none = 0,

    /* A primary prompt line: the start of a prompt, not a continuation. */
    prompt = 1,

    /* A continuation line, marked with k=c. Used to decide that a line
     * belongs to some earlier prompt. */
    prompt_continuation = 2,
};

/* ─── Cell ───────────────────────────────────────────────────────────────── */

struct Cell {
    uint64_t bits;

    Cell() : bits(0) {}

    /* Field positions. Kept as named constants because the tests assert
     * against them, so a field that moves fails loudly rather than quietly
     * reinterpreting existing cells. */
    static const int CONTENT_TAG_SHIFT = 0;
    static const int CONTENT_SHIFT = 2;
    static const int STYLE_ID_SHIFT = 26;
    static const int WIDE_SHIFT = 42;
    static const int PROTECTED_SHIFT = 44;
    static const int HYPERLINK_SHIFT = 45;
    static const int SEMANTIC_SHIFT = 46;

    static const uint64_t CONTENT_TAG_MASK = 0x3;
    static const uint64_t CONTENT_MASK = 0xFFFFFF;      /* 24 bits */
    static const uint64_t STYLE_ID_MASK = 0xFFFF;
    static const uint64_t WIDE_MASK = 0x3;
    static const uint64_t SEMANTIC_MASK = 0x3;

    /* Within the 24-bit content union. */
    static const uint32_t CODEPOINT_MASK = 0x1FFFFF;    /* 21 bits */
    static const uint32_t PALETTE_MASK = 0xFF;

    ContentTag content_tag() const {
        return (ContentTag)((bits >> CONTENT_TAG_SHIFT) & CONTENT_TAG_MASK);
    }
    void set_content_tag(ContentTag t) {
        bits = (bits & ~(CONTENT_TAG_MASK << CONTENT_TAG_SHIFT)) |
               (((uint64_t)t & CONTENT_TAG_MASK) << CONTENT_TAG_SHIFT);
    }

    uint32_t content_raw() const {
        return (uint32_t)((bits >> CONTENT_SHIFT) & CONTENT_MASK);
    }
    void set_content_raw(uint32_t v) {
        bits = (bits & ~(CONTENT_MASK << CONTENT_SHIFT)) |
               (((uint64_t)v & CONTENT_MASK) << CONTENT_SHIFT);
    }

    /* The codepoint, valid when the tag is codepoint or codepoint_grapheme. */
    uint32_t codepoint() const { return content_raw() & CODEPOINT_MASK; }
    void set_codepoint(uint32_t cp) { set_content_raw(cp & CODEPOINT_MASK); }

    /* The palette index, valid when the tag is bg_color_palette. */
    uint8_t color_palette() const { return (uint8_t)(content_raw() & PALETTE_MASK); }
    void set_color_palette(uint8_t idx) { set_content_raw(idx); }

    /* The background color, valid when the tag is bg_color_rgb. */
    RGB color_rgb() const {
        const uint32_t c = content_raw();
        return RGB((uint8_t)(c & 0xFF),
                   (uint8_t)((c >> 8) & 0xFF),
                   (uint8_t)((c >> 16) & 0xFF));
    }
    void set_color_rgb(RGB v) {
        set_content_raw((uint32_t)v.r | ((uint32_t)v.g << 8) | ((uint32_t)v.b << 16));
    }

    /* Index into the page's style set. Zero is the default style, which needs
     * no lookup. */
    style::Id style_id() const {
        return (style::Id)((bits >> STYLE_ID_SHIFT) & STYLE_ID_MASK);
    }
    void set_style_id(style::Id id) {
        bits = (bits & ~(STYLE_ID_MASK << STYLE_ID_SHIFT)) |
               (((uint64_t)id & STYLE_ID_MASK) << STYLE_ID_SHIFT);
    }

    Wide wide() const { return (Wide)((bits >> WIDE_SHIFT) & WIDE_MASK); }
    void set_wide(Wide w) {
        bits = (bits & ~(WIDE_MASK << WIDE_SHIFT)) |
               (((uint64_t)w & WIDE_MASK) << WIDE_SHIFT);
    }

    bool protect() const { return ((bits >> PROTECTED_SHIFT) & 1) != 0; }
    void set_protect(bool v) {
        bits = (bits & ~((uint64_t)1 << PROTECTED_SHIFT)) |
               ((uint64_t)(v ? 1 : 0) << PROTECTED_SHIFT);
    }

    /* Whether this cell belongs to a hyperlink. The ID lives in the page's
     * hyperlink map rather than in the cell. */
    bool hyperlink() const { return ((bits >> HYPERLINK_SHIFT) & 1) != 0; }
    void set_hyperlink(bool v) {
        bits = (bits & ~((uint64_t)1 << HYPERLINK_SHIFT)) |
               ((uint64_t)(v ? 1 : 0) << HYPERLINK_SHIFT);
    }

    SemanticContent semantic_content() const {
        return (SemanticContent)((bits >> SEMANTIC_SHIFT) & SEMANTIC_MASK);
    }
    void set_semantic_content(SemanticContent s) {
        bits = (bits & ~(SEMANTIC_MASK << SEMANTIC_SHIFT)) |
               (((uint64_t)s & SEMANTIC_MASK) << SEMANTIC_SHIFT);
    }

    /* ─── derived ────────────────────────────────────────────────────────── */

    /* True if the cell holds no text. A background-only cell still counts as
     * empty of text. */
    bool has_text() const {
        const ContentTag t = content_tag();
        return (t == ContentTag::codepoint || t == ContentTag::codepoint_grapheme) &&
               codepoint() != 0;
    }

    /* True if the cell participates in a multi-codepoint grapheme cluster. */
    bool has_grapheme() const {
        return content_tag() == ContentTag::codepoint_grapheme;
    }

    /* True if the cell carries a background color of its own, which takes
     * precedence over its style's background. */
    bool has_bg_color() const {
        const ContentTag t = content_tag();
        return t == ContentTag::bg_color_palette || t == ContentTag::bg_color_rgb;
    }

    bool eql(const Cell &o) const { return bits == o.bits; }
};

/* ─── Row ────────────────────────────────────────────────────────────────── */

struct Row {
    uint64_t bits;

    Row() : bits(0) {}

    static const int CELLS_SHIFT = 0;
    static const int WRAP_SHIFT = 32;
    static const int WRAP_CONT_SHIFT = 33;
    static const int GRAPHEME_SHIFT = 34;
    static const int STYLED_SHIFT = 35;
    static const int HYPERLINK_SHIFT = 36;
    static const int SEMANTIC_PROMPT_SHIFT = 37;
    static const int KITTY_PLACEHOLDER_SHIFT = 39;
    static const int DIRTY_SHIFT = 40;

    static const uint64_t CELLS_MASK = 0xFFFFFFFF;
    static const uint64_t SEMANTIC_PROMPT_MASK = 0x3;

    /* The row's cells, as an offset from the page base. */
    Offset<Cell> cells() const {
        Offset<Cell> o;
        o.offset = (OffsetInt)((bits >> CELLS_SHIFT) & CELLS_MASK);
        return o;
    }
    void set_cells(Offset<Cell> o) {
        bits = (bits & ~(CELLS_MASK << CELLS_SHIFT)) |
               (((uint64_t)o.offset & CELLS_MASK) << CELLS_SHIFT);
    }

    /* Soft-wrapped: the next row continues this one. */
    bool wrap() const { return bit(WRAP_SHIFT); }
    void set_wrap(bool v) { set_bit(WRAP_SHIFT, v); }

    /* This row continues the previous one. */
    bool wrap_continuation() const { return bit(WRAP_CONT_SHIFT); }
    void set_wrap_continuation(bool v) { set_bit(WRAP_CONT_SHIFT, v); }

    /* Some cell here has a multi-codepoint grapheme cluster, so the fast
     * paths that skip grapheme cleanup are unavailable. */
    bool grapheme() const { return bit(GRAPHEME_SHIFT); }
    void set_grapheme(bool v) { set_bit(GRAPHEME_SHIFT, v); }

    /* Some cell here uses a ref-counted style.
     *
     * False positives are allowed, false negatives are not: it is set the
     * first time a style is used and never cleared, because checking whether
     * a style is still in use would cost more than it saves. Erase operations
     * use it to skip style cleanup entirely on rows that were never styled,
     * which upstream measures at roughly 4x. */
    bool styled() const { return bit(STYLED_SHIFT); }
    void set_styled(bool v) { set_bit(STYLED_SHIFT, v); }

    /* Some cell here is part of a hyperlink. Same false-positive rule as
     * styled. */
    bool hyperlink() const { return bit(HYPERLINK_SHIFT); }
    void set_hyperlink(bool v) { set_bit(HYPERLINK_SHIFT, v); }

    /* Whether this row holds prompt cells. Only an optimization for
     * jump-to-prompt; individual cells still have to be checked. False
     * positives are allowed, false negatives are not. */
    SemanticPrompt semantic_prompt() const {
        return (SemanticPrompt)((bits >> SEMANTIC_PROMPT_SHIFT) & SEMANTIC_PROMPT_MASK);
    }
    void set_semantic_prompt(SemanticPrompt p) {
        bits = (bits & ~(SEMANTIC_PROMPT_MASK << SEMANTIC_PROMPT_SHIFT)) |
               (((uint64_t)p & SEMANTIC_PROMPT_MASK) << SEMANTIC_PROMPT_SHIFT);
    }

    /* Holds a Kitty graphics virtual placeholder (U+10EEEE). The bit is kept
     * even when Kitty graphics are disabled so the layout does not change. */
    bool kitty_virtual_placeholder() const { return bit(KITTY_PLACEHOLDER_SHIFT); }
    void set_kitty_virtual_placeholder(bool v) { set_bit(KITTY_PLACEHOLDER_SHIFT, v); }

    /* Needs redrawing. Set by anything that changes the row's contents or
     * position, and cleared by whoever draws it.
     *
     * Conveys only that something changed visually. False positives are
     * allowed; a false negative leaves an artifact on screen. */
    bool dirty() const { return bit(DIRTY_SHIFT); }
    void set_dirty(bool v) { set_bit(DIRTY_SHIFT, v); }

private:
    bool bit(int shift) const { return ((bits >> shift) & 1) != 0; }
    void set_bit(int shift, bool v) {
        bits = (bits & ~((uint64_t)1 << shift)) | ((uint64_t)(v ? 1 : 0) << shift);
    }
};

/* ─── page storage ───────────────────────────────────────────────────────── */

/* The cell array starts on a cache line so that a row's cells never begin
 * mid-line and straddle an extra one. */
static const size_t CELLS_ALIGN =
    alignof(Cell) > 64 ? alignof(Cell) : 64;

/* Grapheme codepoints past the first are stored as u32s, so a 4-byte chunk
 * wastes nothing. */
typedef BitmapAllocator<4> GraphemeAlloc;

/* Byte storage for hyperlink URIs and other page strings. */
typedef BitmapAllocator<8> StringAlloc;

/* Where a cell's extra grapheme codepoints live within grapheme_alloc. */
struct GraphemeSlice {
    OffsetInt offset;
    uint32_t  len;

    GraphemeSlice() : offset(0), len(0) {}
    bool operator==(const GraphemeSlice &o) const {
        return offset == o.offset && len == o.len;
    }
};

/* Maps a cell — keyed by its offset from the page base — to its grapheme
 * data. Keyed by raw offset rather than Offset<Cell> because the key only
 * needs to compare equal, and a plain integer does that without dragging
 * operator== onto Offset. */
typedef OffsetHashMap<OffsetInt, GraphemeSlice> GraphemeMap;

/* Default sizes for the variable-length regions. Each is a starting point
 * that a page grows past by being reallocated at a larger capacity, not a
 * hard ceiling on what a terminal can display. */
static const GraphemeBytesInt GRAPHEME_BYTES_DEFAULT = 512;
static const StringBytesInt   STRING_BYTES_DEFAULT = 512;
static const HyperlinkCountInt HYPERLINK_BYTES_DEFAULT = 512;

/* How much of a page is given over to each thing. */
struct Capacity {
    CellCountInt cols;
    CellCountInt rows;

    /* Distinct styles usable on this page. */
    StyleCountInt styles;

    /* Rough byte budget for hyperlinks. Actual usage runs higher, since
     * hyperlinks also consume string bytes and lookup metadata. */
    HyperlinkCountInt hyperlink_bytes;

    GraphemeBytesInt grapheme_bytes;
    StringBytesInt   string_bytes;

    Capacity()
        : cols(0), rows(0), styles(16),
          hyperlink_bytes(HYPERLINK_BYTES_DEFAULT),
          grapheme_bytes(GRAPHEME_BYTES_DEFAULT),
          string_bytes(STRING_BYTES_DEFAULT) {}

    Capacity(CellCountInt c, CellCountInt r) : Capacity() { cols = c; rows = r; }
};

/* The memory layout of a page.
 *
 * Laid out as row headers, then the cell array, then the metadata block:
 *
 *   [rows][cells][styles, graphemes, strings]
 *
 * Row headers start at offset zero and the cell array is cache-line aligned.
 * Only the row headers need initializing: cells and every metadata member
 * treat all-zero as their empty state, so the pages behind everything past
 * the row headers stay untouched until first use.
 *
 * The hyperlink regions sit at the end. A cell that is part of a link carries
 * only a bit; the map turns that cell's offset into a link ID, and the set
 * holds the link data itself so a run of cells under one OSC 8 sequence
 * shares one entry.
 */
struct PageLayout {
    size_t total_size;

    size_t rows_start;
    size_t rows_size;

    size_t cells_start;
    size_t cells_size;

    size_t              styles_start;
    style::Set::Layout  styles_layout;

    size_t                 grapheme_alloc_start;
    GraphemeAlloc::Layout  grapheme_alloc_layout;

    size_t             grapheme_map_start;
    GraphemeMap::Layout grapheme_map_layout;

    size_t              string_alloc_start;
    StringAlloc::Layout string_alloc_layout;

    size_t                 hyperlink_map_start;
    hyperlink::Map::Layout hyperlink_map_layout;

    size_t                 hyperlink_set_start;
    hyperlink::Set::Layout hyperlink_set_layout;

    Capacity capacity;

    static PageLayout init(const Capacity &cap) {
        PageLayout l;
        l.capacity = cap;

        l.rows_start = 0;
        l.rows_size = (size_t)cap.rows * sizeof(Row);
        const size_t rows_end = l.rows_start + l.rows_size;

        l.cells_start = align_forward(rows_end, CELLS_ALIGN);
        l.cells_size = (size_t)cap.rows * (size_t)cap.cols * sizeof(Cell);
        const size_t cells_end = l.cells_start + l.cells_size;

        /* The metadata block. Each member publishes its own layout, and each
         * starts aligned for what it holds. */
        l.styles_layout = style::Set::Layout::init(
            style::Set::capacity_for_count(cap.styles));
        l.styles_start = align_forward(cells_end, style::Set::base_align);
        const size_t styles_end = l.styles_start + l.styles_layout.total_size;

        l.grapheme_alloc_layout = GraphemeAlloc::layout(cap.grapheme_bytes);
        l.grapheme_alloc_start = align_forward(styles_end, GraphemeAlloc::base_align);
        const size_t grapheme_alloc_end =
            l.grapheme_alloc_start + l.grapheme_alloc_layout.total_size;

        /* One grapheme map entry per cell that could carry graphemes, bounded
         * by what the grapheme bytes could actually describe. */
        l.grapheme_map_layout = GraphemeMap::Layout::init(
            GraphemeMap::capacity_for_count(cap.grapheme_bytes / 4));
        l.grapheme_map_start = align_forward(grapheme_alloc_end, alignof(uint64_t));
        const size_t grapheme_map_end =
            l.grapheme_map_start + l.grapheme_map_layout.total_size;

        l.string_alloc_layout = StringAlloc::layout(cap.string_bytes);
        l.string_alloc_start = align_forward(grapheme_map_end, StringAlloc::base_align);
        const size_t string_alloc_end =
            l.string_alloc_start + l.string_alloc_layout.total_size;

        /* Many cells can share one link, so the map scales with cells while
         * the set scales with the byte budget. Sizing the map to every cell
         * would roughly double a page for something almost no cell uses, so
         * it is bounded at an eighth of them with a floor — a page with more
         * distinct linked cells than that refuses the extra links rather
         * than growing, and the text still renders. */
        {
            const size_t cells = (size_t)cap.rows * (size_t)cap.cols;
            size_t map_entries = cells / 8;
            if (map_entries < 64) map_entries = 64;

            l.hyperlink_map_layout = hyperlink::Map::Layout::init(
                hyperlink::Map::capacity_for_count((uint32_t)map_entries));
            l.hyperlink_map_start = align_forward(string_alloc_end, alignof(uint64_t));
            const size_t hyperlink_map_end =
                l.hyperlink_map_start + l.hyperlink_map_layout.total_size;

            /* A link needs a URI, so the byte budget bounds how many can
             * exist. Sixteen bytes is a short but plausible URI. */
            size_t set_entries = (size_t)cap.hyperlink_bytes / 16;
            if (set_entries < 8) set_entries = 8;

            l.hyperlink_set_layout = hyperlink::Set::Layout::init(
                hyperlink::Set::capacity_for_count(set_entries));
            l.hyperlink_set_start =
                align_forward(hyperlink_map_end, hyperlink::Set::base_align);

            l.total_size = l.hyperlink_set_start + l.hyperlink_set_layout.total_size;
        }

        return l;
    }

    /* Bytes the grid occupies — the row headers plus the cell array. Used
     * when refitting a capacity into a fixed allocation. */
    static size_t grid_bytes(const Capacity &cap) {
        const PageLayout l = init(cap);
        return l.rows_size + l.cells_size;
    }
};

/* The widest grid that fits this capacity without growing it, or 0 if not
 * even one column fits.
 *
 * A single row's header takes a whole cell-aligned region ahead of the cells,
 * so that comes off the top before dividing what is left by the cell size. */
inline CellCountInt capacity_max_cols(const Capacity &cap) {
    const PageLayout l = PageLayout::init(cap);
    const size_t grid = l.rows_size + l.cells_size;

    const size_t row_region = align_forward(sizeof(Row), CELLS_ALIGN);
    if (grid <= row_region) return 0;

    const size_t max_cols = (grid - row_region) / sizeof(Cell);
    const size_t clamp = (size_t)(CellCountInt)-1;
    return (CellCountInt)(max_cols < clamp ? max_cols : clamp);
}

/* Refit a capacity to a new column count without growing the allocation.
 *
 * Only the row count gives; everything else may grow. Because the cell array
 * is cache-line aligned, the padding between the row headers and the cells
 * depends on the row count, so rows are trimmed until the layout fits. That
 * padding is under one cache line, so this settles in a few iterations.
 *
 * Returns false if no row count fits. */
inline bool capacity_adjust_cols(const Capacity &in, CellCountInt cols, Capacity *out) {
    const size_t total_size = PageLayout::init(in).total_size;

    const PageLayout l = PageLayout::init(in);
    const size_t grid = l.rows_size + l.cells_size;

    const size_t bytes_per_row = sizeof(Row) + sizeof(Cell) * (size_t)cols;
    if (bytes_per_row == 0) return false;

    Capacity adjusted = in;
    adjusted.cols = cols;

    size_t new_rows = grid / bytes_per_row;
    while (new_rows > 0) {
        adjusted.rows = (CellCountInt)new_rows;
        if (PageLayout::init(adjusted).total_size <= total_size) {
            *out = adjusted;
            return true;
        }
        new_rows--;
    }

    return false;
}

/* ─── Page ───────────────────────────────────────────────────────────────── */

/* A self-contained section of terminal screen.
 *
 * Everything a page needs lives in one allocation: the row headers, the cell
 * array, the interned style set, and the grapheme and string storage. Nothing
 * inside holds an absolute pointer — every internal reference is a byte offset
 * from the allocation base — so a page can be memcpy'd to a different address,
 * written to disk, or moved between allocations and still work. That is what
 * `relocate` demonstrates, and it is the reason for the offset machinery
 * underneath all of this.
 *
 * PARTIAL. This is the storage layer: construction, addressing cells and rows,
 * and style assignment with its reference counting. The larger operations from
 * upstream's page.zig — resize, clone, scrolling, grapheme and hyperlink
 * management — are not here yet.
 */
struct Page {
    /* Base of the backing allocation. The only absolute pointer, and it is
     * deliberately outside the page's own memory so relocating is just
     * changing this field. */
    uint8_t *memory;

    Offset<Row>  rows;
    Offset<Cell> cells;

    style::Set    styles;
    GraphemeAlloc grapheme_alloc;
    GraphemeMap   grapheme_map;
    StringAlloc   string_alloc;

    /* Cells that are part of a link, and the link data itself. A cell holds
       only a bit, so the map turns a linked cell's offset into an ID. */
    hyperlink::Map hyperlink_map;
    hyperlink::Set hyperlink_set;

    Capacity capacity;
    size_t   size;

    Page() : memory(nullptr), rows(), cells(), capacity(), size(0) {}

    /* Build a page in caller-provided memory, which must be at least
     * PageLayout::init(cap).total_size bytes and zero-filled.
     *
     * Only the row headers are written: every other member treats all-zero as
     * its empty state, so a fresh page leaves the rest of the pages untouched
     * until something actually uses them. */
    static Page init(uint8_t *buf, const Capacity &cap) {
        const PageLayout l = PageLayout::init(cap);
        OffsetBuf base = OffsetBuf::init(buf);

        Page p;
        p.memory = buf;
        p.capacity = cap;
        p.size = l.total_size;

        p.rows = base.member<Row>(l.rows_start);
        p.cells = base.member<Cell>(l.cells_start);

        p.styles = style::Set::init_assume_zeroed(
            base.add(l.styles_start), l.styles_layout, style::Context());
        p.grapheme_alloc = GraphemeAlloc::init_assume_zeroed(
            base.add(l.grapheme_alloc_start), l.grapheme_alloc_layout);
        p.grapheme_map = GraphemeMap::init_assume_zeroed(
            base.add(l.grapheme_map_start), l.grapheme_map_layout);
        p.string_alloc = StringAlloc::init_assume_zeroed(
            base.add(l.string_alloc_start), l.string_alloc_layout);

        p.hyperlink_map = hyperlink::Map::init_assume_zeroed(
            base.add(l.hyperlink_map_start), l.hyperlink_map_layout);
        p.hyperlink_set = hyperlink::Set::init_assume_zeroed(
            base.add(l.hyperlink_set_start), l.hyperlink_set_layout,
            hyperlink::Context());

        /* Point each row at its slice of the cell array. This is the one thing
         * a zeroed buffer cannot express, since row 0's cells are not at
         * offset 0. */
        Row *r = p.rows.ptr(buf);
        for (CellCountInt y = 0; y < cap.rows; y++) {
            Offset<Cell> o;
            o.offset = (OffsetInt)(l.cells_start +
                                   (size_t)y * cap.cols * sizeof(Cell));
            r[y].set_cells(o);
        }

        return p;
    }

    /* Move the page to a new base address.
     *
     * The caller copies `size` bytes; this just re-points the base. Nothing
     * else needs fixing up, which is the entire payoff of offset addressing. */
    void relocate(uint8_t *new_base) { memory = new_base; }

    /* ─── addressing ─────────────────────────────────────────────────────── */

    Row *get_row(CellCountInt y) {
        assert(y < capacity.rows);
        return &rows.ptr(memory)[y];
    }

    const Row *get_row(CellCountInt y) const {
        assert(y < capacity.rows);
        return &rows.ptr(memory)[y];
    }

    /* The cells of a row, `capacity.cols` of them. */
    Cell *get_cells(CellCountInt y) {
        return get_row(y)->cells().ptr(memory);
    }

    Cell *get_cell(CellCountInt x, CellCountInt y) {
        assert(x < capacity.cols);
        return &get_cells(y)[x];
    }

    /* ─── styles ─────────────────────────────────────────────────────────── */

    /* Assign a style to a cell, taking a reference to the new style and
     * releasing the old one.
     *
     * The new reference is taken before the old is released so that
     * re-applying the same style is a no-op rather than briefly dropping the
     * refcount to zero, which would make the style eligible for reaping
     * between the two calls.
     *
     * Returns false if the style set is full. */
    bool set_cell_style(CellCountInt x, CellCountInt y, const style::Style &s) {
        Cell *c = get_cell(x, y);
        const style::Id old_id = c->style_id();

        style::Id new_id = style::DEFAULT_ID;
        if (!s.is_default()) {
            if (styles.add(memory, s, &new_id) != AddResult::ok) return false;
        }

        if (old_id != style::DEFAULT_ID) styles.release(memory, old_id);

        c->set_style_id(new_id);

        if (new_id != style::DEFAULT_ID) {
            /* The row's styled flag is a false-positive-only hint: set once a
             * style is used, never cleared, because proving no cell is still
             * styled would cost more than the flag saves. */
            get_row(y)->set_styled(true);
        }

        return true;
    }

    /* The style of a cell, or the default when it has none. */
    style::Style get_cell_style(CellCountInt x, CellCountInt y) {
        const style::Id id = get_cell(x, y)->style_id();
        if (id == style::DEFAULT_ID) return style::Style();
        return *styles.get(memory, id);
    }

    /* Drop a cell's style reference and reset it to default. Used when
     * erasing, so that styles do not leak references. */
    void clear_cell_style(CellCountInt x, CellCountInt y) {
        Cell *c = get_cell(x, y);
        const style::Id id = c->style_id();
        if (id == style::DEFAULT_ID) return;
        styles.release(memory, id);
        c->set_style_id(style::DEFAULT_ID);
    }

    /* Erase a cell entirely, releasing anything it referenced. */
    void clear_cell(CellCountInt x, CellCountInt y) {
        clear_cell_style(x, y);
        *get_cell(x, y) = Cell();
    }

    /* Number of distinct styles currently interned. */
    size_t style_count() const { return styles.count(); }
};

/* ─── closing the hyperlink/page cycle ───────────────────────────────────── */

/* Declared in hyperlink.hpp against a forward-declared Page. */
inline void hyperlink::PageEntry::free(Page *page) const {
    if (!page) return;

    /* An explicit ID is its own allocation; an implicit one is just a
     * counter and owns nothing. */
    if (kind == IdKind::explicit_id && explicit_id.len > 0) {
        page->string_alloc.free(page->memory,
                                explicit_id.offset.ptr(page->memory),
                                explicit_id.len);
    }
    if (uri.len > 0) {
        page->string_alloc.free(page->memory, uri.offset.ptr(page->memory), uri.len);
    }
}

/* ─── page hyperlink operations ──────────────────────────────────────────── */

/* Hash for map keys that are cell offsets, used by both the grapheme map
 * and the hyperlink map.
 *
 * A cell offset is always a multiple of sizeof(Cell), so its low bits are
 * zero. The map takes its bucket index from the low bits of the hash, so
 * using the offset directly would leave most buckets unreachable and pile
 * every key into a fraction of the table. Mixing first spreads them. */
struct PageCellKeyHash {
    uint64_t operator()(OffsetInt off) const {
        uint64_t x = (uint64_t)off + 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }
};

inline uint64_t page_cell_key_hash(OffsetInt off) {
    return PageCellKeyHash()(off);
}

/* Copy bytes into the page's string storage, returning a slice.
 * Returns false if there is no room. */
inline bool page_alloc_string(Page *p, const char *s, size_t len,
                              Offset<uint8_t>::Slice *out) {
    out->len = 0;
    out->offset.offset = 0;
    if (len == 0) return true;

    uint8_t *dst = p->string_alloc.alloc<uint8_t>(p->memory, len);
    if (!dst) return false;

    memcpy(dst, s, len);
    out->offset = get_offset<uint8_t>(p->memory, dst);
    out->len = len;
    return true;
}

/* Attach a hyperlink to a cell.
 *
 * `id` may be null, in which case `implicit` distinguishes this link from
 * others — that is what keeps two separate OSC 8 runs to the same URI from
 * being treated as one link.
 *
 * Returns false and changes nothing if the strings or the map have no room.
 * The text still renders; only the link is dropped. */
inline bool page_set_cell_hyperlink(Page *p, CellCountInt x, CellCountInt y,
                                    const char *uri, size_t uri_len,
                                    const char *id, size_t id_len,
                                    OffsetInt implicit) {
    if (!p || !uri || uri_len == 0) return false;

    /* The set resolves offsets through the page, so it needs to know which
     * page before any lookup or insert. */
    p->hyperlink_set.context.base = p->memory;
    p->hyperlink_set.context.src_base = nullptr;
    p->hyperlink_set.context.page = p;

    hyperlink::PageEntry entry;
    if (!page_alloc_string(p, uri, uri_len, &entry.uri)) return false;

    if (id && id_len > 0) {
        entry.kind = hyperlink::PageEntry::IdKind::explicit_id;
        if (!page_alloc_string(p, id, id_len, &entry.explicit_id)) {
            /* Undo the URI, or it leaks for the page's lifetime. */
            p->string_alloc.free(p->memory, entry.uri.offset.ptr(p->memory),
                                 entry.uri.len);
            return false;
        }
    } else {
        entry.kind = hyperlink::PageEntry::IdKind::implicit_id;
        entry.implicit_id = implicit;
    }

    /* If an identical link is already interned, add() reports it through the
     * context's deleted hook, which releases the strings just allocated. */
    hyperlink::Id link_id = 0;
    if (p->hyperlink_set.add(p->memory, entry, &link_id) != AddResult::ok) {
        entry.free(p);
        return false;
    }

    Cell *c = p->get_cell(x, y);
    const OffsetInt cell_off = get_offset<Cell>(p->memory, c).offset;

    if (!p->hyperlink_map.put(p->memory, page_cell_key_hash(cell_off),
                              cell_off, link_id)) {
        p->hyperlink_set.release(p->memory, link_id);
        return false;
    }

    c->set_hyperlink(true);
    p->get_row(y)->set_hyperlink(true);
    return true;
}

/* The URI attached to a cell, or false if it has none.
 *
 * The returned pointer is into page memory and is invalidated by anything
 * that frees the link. */
inline bool page_get_cell_hyperlink(Page *p, CellCountInt x, CellCountInt y,
                                    const uint8_t **uri, size_t *uri_len) {
    if (!p) return false;

    Cell *c = p->get_cell(x, y);
    if (!c->hyperlink()) return false;

    const OffsetInt cell_off = get_offset<Cell>(p->memory, c).offset;
    hyperlink::Id *link_id = p->hyperlink_map.get(
        p->memory, page_cell_key_hash(cell_off), cell_off);
    if (!link_id) return false;

    p->hyperlink_set.context.base = p->memory;
    p->hyperlink_set.context.src_base = nullptr;
    p->hyperlink_set.context.page = p;

    hyperlink::PageEntry *e = p->hyperlink_set.get(p->memory, *link_id);
    if (!e) return false;

    *uri = e->uri.offset.ptr(p->memory);
    *uri_len = e->uri.len;
    return true;
}

/* Detach a cell's hyperlink, releasing the link if this was its last cell. */
inline void page_clear_cell_hyperlink(Page *p, CellCountInt x, CellCountInt y) {
    if (!p) return;

    Cell *c = p->get_cell(x, y);
    if (!c->hyperlink()) return;

    const OffsetInt cell_off = get_offset<Cell>(p->memory, c).offset;
    const uint64_t h = page_cell_key_hash(cell_off);

    hyperlink::Id *link_id = p->hyperlink_map.get(p->memory, h, cell_off);
    if (link_id) {
        p->hyperlink_set.context.base = p->memory;
        p->hyperlink_set.context.src_base = nullptr;
        p->hyperlink_set.context.page = p;

        p->hyperlink_set.release(p->memory, *link_id);
        p->hyperlink_map.remove(p->memory, h, cell_off, PageCellKeyHash());
    }

    /* The row's hyperlink flag is a false-positive-only hint like styled, so
     * it is deliberately not cleared here. */
    c->set_hyperlink(false);
}

/* ─── page grapheme operations ───────────────────────────────────────────── */

/* A cell holds one codepoint. Anything built from several — a base letter
 * plus combining marks, a flag, an emoji with a modifier — keeps its first
 * codepoint in the cell and the rest in the page's grapheme storage, found by
 * the cell's offset.
 *
 * Splitting it this way keeps the common case free: a cell of plain text
 * costs nothing extra, and only cells that actually need more pay for it. The
 * cell's tag says which case it is, and the row carries a flag so operations
 * that erase cells can skip grapheme cleanup entirely on rows that never had
 * any.
 *
 * Appending reallocates, which is quadratic in the number of codepoints on
 * one cell. Clusters are two or three codepoints in practice, so the simpler
 * code is worth more than the saved copies. */

/* The additional codepoints on a cell, beyond the one it holds directly.
 * Returns false if the cell has none. */
inline bool page_grapheme_codepoints(Page *p, CellCountInt x, CellCountInt y,
                                     const uint32_t **out, uint32_t *out_len) {
    if (!p) return false;

    Cell *c = p->get_cell(x, y);
    if (!c->has_grapheme()) return false;

    const OffsetInt cell_off = get_offset<Cell>(p->memory, c).offset;
    GraphemeSlice *slice = p->grapheme_map.get(
        p->memory, page_cell_key_hash(cell_off), cell_off);
    if (!slice || slice->len == 0) return false;

    Offset<uint32_t> o;
    o.offset = slice->offset;
    *out = o.ptr(p->memory);
    *out_len = slice->len;
    return true;
}

/* Append a codepoint to a cell's grapheme cluster.
 *
 * Returns false and changes nothing if the grapheme storage or map is full;
 * the cell keeps the codepoints it already had and still renders. */
inline bool page_append_grapheme(Page *p, CellCountInt x, CellCountInt y,
                                 uint32_t cp) {
    if (!p) return false;

    Cell *c = p->get_cell(x, y);
    const OffsetInt cell_off = get_offset<Cell>(p->memory, c).offset;
    const uint64_t h = page_cell_key_hash(cell_off);

    const uint32_t *old_cps = nullptr;
    uint32_t old_len = 0;
    GraphemeSlice *existing = p->grapheme_map.get(p->memory, h, cell_off);
    if (existing && existing->len > 0) {
        Offset<uint32_t> o;
        o.offset = existing->offset;
        old_cps = o.ptr(p->memory);
        old_len = existing->len;
    }

    uint32_t *dst = p->grapheme_alloc.alloc<uint32_t>(p->memory, old_len + 1);
    if (!dst) return false;

    for (uint32_t i = 0; i < old_len; i++) dst[i] = old_cps[i];
    dst[old_len] = cp;

    GraphemeSlice slice;
    slice.offset = get_offset<uint32_t>(p->memory, dst).offset;
    slice.len = old_len + 1;

    if (!p->grapheme_map.put(p->memory, h, cell_off, slice)) {
        p->grapheme_alloc.free(p->memory, dst, old_len + 1);
        return false;
    }

    /* The old run is only released once the new one is safely in the map, so
     * a failure above leaves the cell exactly as it was. */
    if (old_len > 0) {
        p->grapheme_alloc.free(p->memory, const_cast<uint32_t *>(old_cps), old_len);
    }

    c->set_content_tag(ContentTag::codepoint_grapheme);
    p->get_row(y)->set_grapheme(true);
    return true;
}

/* Drop a cell's extra codepoints, leaving the one in the cell itself. */
inline void page_clear_grapheme(Page *p, CellCountInt x, CellCountInt y) {
    if (!p) return;

    Cell *c = p->get_cell(x, y);
    if (!c->has_grapheme()) return;

    const OffsetInt cell_off = get_offset<Cell>(p->memory, c).offset;
    const uint64_t h = page_cell_key_hash(cell_off);

    GraphemeSlice *slice = p->grapheme_map.get(p->memory, h, cell_off);
    if (slice && slice->len > 0) {
        Offset<uint32_t> o;
        o.offset = slice->offset;
        p->grapheme_alloc.free(p->memory, o.ptr(p->memory), slice->len);
    }
    p->grapheme_map.remove(p->memory, h, cell_off, PageCellKeyHash());

    /* Back to an ordinary single-codepoint cell. The row's grapheme flag is a
     * false-positive-only hint, so it stays set. */
    c->set_content_tag(ContentTag::codepoint);
}

/* ─── erasing ────────────────────────────────────────────────────────────── */

/* Erase a cell, releasing everything it referenced.
 *
 * Page::clear_cell only knows about styles, because graphemes and hyperlinks
 * are reached through free functions declared after Page. This is the
 * complete version and is what erase paths should call — dropping a cell
 * without releasing its grapheme run or its link would strand both for the
 * page's lifetime. */
inline void page_erase_cell(Page *p, CellCountInt x, CellCountInt y) {
    if (!p) return;
    page_clear_grapheme(p, x, y);
    page_clear_cell_hyperlink(p, x, y);
    p->clear_cell_style(x, y);
    *p->get_cell(x, y) = Cell();
}

/* Erase a whole row. */
inline void page_erase_row(Page *p, CellCountInt y) {
    if (!p) return;

    /* The row flags are false-positive-only, so a row that never held a style,
     * grapheme or link can skip the per-cell release work entirely. This is
     * why those flags exist. */
    Row *row = p->get_row(y);
    const bool needs_release =
        row->styled() || row->grapheme() || row->hyperlink();

    if (needs_release) {
        for (CellCountInt x = 0; x < p->capacity.cols; x++) {
            page_erase_cell(p, x, y);
        }
    } else {
        Cell *cells = p->get_cells(y);
        for (CellCountInt x = 0; x < p->capacity.cols; x++) cells[x] = Cell();
    }

    row->set_dirty(true);
}

/* ─── cloning ────────────────────────────────────────────────────────────── */

/* Copy a hyperlink entry's strings into another page. Declared in
 * hyperlink.hpp; defined here because it needs Page and the string storage. */
inline bool hyperlink::PageEntry::dupe(const uint8_t *self_base, Page *dst,
                                       PageEntry *out) const {
    if (!dst || !out) return false;

    PageEntry copy;
    copy.kind = kind;
    copy.implicit_id = implicit_id;

    if (!page_alloc_string(dst, (const char *)uri.offset.ptr(self_base),
                           uri.len, &copy.uri)) {
        return false;
    }

    if (kind == IdKind::explicit_id && explicit_id.len > 0) {
        if (!page_alloc_string(dst,
                               (const char *)explicit_id.offset.ptr(self_base),
                               explicit_id.len, &copy.explicit_id)) {
            /* Leave the destination exactly as it was rather than stranding a
             * URI nothing points at. */
            dst->string_alloc.free(dst->memory, copy.uri.offset.ptr(dst->memory),
                                   copy.uri.len);
            return false;
        }
    }

    *out = copy;
    return true;
}

/* Copy one cell's hyperlink from another page, interning it in the
 * destination's set and returning the destination's ID for it.
 *
 * The lookup runs before the strings are duplicated, using the source page as
 * the hash and comparison base — that is what src_base on the context is for.
 * A link shared by a run of cells therefore costs one copy of its strings, not
 * one per cell. */
inline bool page_clone_cell_hyperlink(Page *dst, Page *src,
                                      const hyperlink::PageEntry &src_entry,
                                      hyperlink::Id *out_id) {
    dst->hyperlink_set.context.base = dst->memory;
    dst->hyperlink_set.context.src_base = src->memory;
    dst->hyperlink_set.context.page = dst;

    if (dst->hyperlink_set.lookup(dst->memory, src_entry) != 0) {
        /* Already interned here. add() takes the reference through the one
         * code path that knows about resurrecting a dead entry, so it is used
         * rather than touching the count directly. It cannot insert the
         * source's entry verbatim, since the lookup it repeats just found a
         * match. */
        hyperlink::Id id = 0;
        const AddResult r = dst->hyperlink_set.add(dst->memory, src_entry, &id);
        dst->hyperlink_set.context.src_base = nullptr;
        if (r != AddResult::ok) return false;
        *out_id = id;
        return true;
    }

    /* Genuinely new here, so the strings have to exist in the destination
     * before the entry describing them does. */
    hyperlink::PageEntry copy;
    if (!src_entry.dupe(src->memory, dst, &copy)) {
        dst->hyperlink_set.context.src_base = nullptr;
        return false;
    }

    dst->hyperlink_set.context.src_base = nullptr;
    hyperlink::Id id = 0;
    if (dst->hyperlink_set.add(dst->memory, copy, &id) != AddResult::ok) {
        copy.free(dst);
        return false;
    }

    *out_id = id;
    return true;
}

/* Copy a single cell from one page to another, translating everything it
 * refers to.
 *
 * Everything a cell points at is page-relative, so a clone is not a memcpy:
 * the style ID indexes the source's style set, the hyperlink ID the source's
 * link set, and the grapheme run lives in the source's storage. Each has to
 * be re-interned in the destination, which may hand back a different ID.
 *
 * The destination cell is erased first, so this may be used to overwrite a
 * cell that already holds something. On failure it is erased again and false
 * returned, leaving no half-translated cell behind.
 *
 * Cells rather than rows are the unit here because reflow moves cells to
 * different columns and different rows than they came from. */
inline bool page_clone_cell(Page *dst, CellCountInt dst_x, CellCountInt dst_y,
                            Page *src, CellCountInt src_x, CellCountInt src_y) {
    if (!dst || !src) return false;
    if (dst_x >= dst->capacity.cols || dst_y >= dst->capacity.rows) return false;
    if (src_x >= src->capacity.cols || src_y >= src->capacity.rows) return false;

    page_erase_cell(dst, dst_x, dst_y);

    Cell *sc = src->get_cell(src_x, src_y);
    Cell *dc = dst->get_cell(dst_x, dst_y);
    Row *dst_row = dst->get_row(dst_y);
    *dc = *sc;

    if (sc->style_id() != style::DEFAULT_ID) {
        style::Style *s = src->styles.get(src->memory, sc->style_id());
        if (!s) {
            page_erase_cell(dst, dst_x, dst_y);
            return false;
        }
        style::Id new_id = style::DEFAULT_ID;
        if (dst->styles.add(dst->memory, *s, &new_id) != AddResult::ok) {
            page_erase_cell(dst, dst_x, dst_y);
            return false;
        }
        dc->set_style_id(new_id);
        dst_row->set_styled(true);
    }

    if (sc->has_grapheme()) {
        const uint32_t *cps = nullptr;
        uint32_t len = 0;
        /* Reset the tag first: the copied cell claims a cluster the
         * destination has not stored yet, and append sets it back. */
        dc->set_content_tag(ContentTag::codepoint);
        if (page_grapheme_codepoints(src, src_x, src_y, &cps, &len)) {
            for (uint32_t i = 0; i < len; i++) {
                if (!page_append_grapheme(dst, dst_x, dst_y, cps[i])) {
                    page_erase_cell(dst, dst_x, dst_y);
                    return false;
                }
            }
        }
    }

    if (sc->hyperlink()) {
        const OffsetInt src_off = get_offset<Cell>(src->memory, sc).offset;
        hyperlink::Id *src_id = src->hyperlink_map.get(
            src->memory, page_cell_key_hash(src_off), src_off);

        /* The bit is only a hint that the map may have an entry; a cell whose
         * link has gone simply arrives without one. */
        dc->set_hyperlink(false);
        if (!src_id) return true;

        src->hyperlink_set.context.base = src->memory;
        src->hyperlink_set.context.src_base = nullptr;
        src->hyperlink_set.context.page = src;
        hyperlink::PageEntry *se = src->hyperlink_set.get(src->memory, *src_id);
        if (!se) return true;

        hyperlink::Id new_id = 0;
        if (!page_clone_cell_hyperlink(dst, src, *se, &new_id)) {
            page_erase_cell(dst, dst_x, dst_y);
            return false;
        }

        const OffsetInt dst_off = get_offset<Cell>(dst->memory, dc).offset;
        if (!dst->hyperlink_map.put(dst->memory, page_cell_key_hash(dst_off),
                                    dst_off, new_id)) {
            dst->hyperlink_set.release(dst->memory, new_id);
            page_erase_cell(dst, dst_x, dst_y);
            return false;
        }

        dc->set_hyperlink(true);
        dst_row->set_hyperlink(true);
    }

    return true;
}

/* Copy a single row from one page to another.
 *
 * On failure the destination row is erased and false returned, so a caller
 * that runs out of destination capacity is left with a clean row rather than
 * a half-translated one. */
inline bool page_clone_row(Page *dst, CellCountInt dst_y,
                           Page *src, CellCountInt src_y) {
    if (!dst || !src) return false;
    if (dst_y >= dst->capacity.rows || src_y >= src->capacity.rows) return false;
    if (dst->capacity.cols < src->capacity.cols) return false;

    page_erase_row(dst, dst_y);

    Row *dst_row = dst->get_row(dst_y);
    const Row *src_row = src->get_row(src_y);

    /* The row's flags come across, but its cells offset belongs to the
     * destination's own allocation and must not. */
    const Offset<Cell> dst_cells_off = dst_row->cells();
    *dst_row = *src_row;
    dst_row->set_cells(dst_cells_off);
    dst_row->set_dirty(true);

    for (CellCountInt x = 0; x < src->capacity.cols; x++) {
        if (!page_clone_cell(dst, x, dst_y, src, x, src_y)) {
            page_erase_row(dst, dst_y);
            return false;
        }
    }

    /* Columns past the source's width belong to the destination and are not
     * described by the copied row, so they are left erased. */
    return true;
}

/* Copy a run of rows. Rows already written into the destination are kept on
 * failure — the caller knows how many succeeded from the return value. */
inline CellCountInt page_clone_rows(Page *dst, CellCountInt dst_y,
                                    Page *src, CellCountInt src_y,
                                    CellCountInt count) {
    CellCountInt done = 0;
    for (; done < count; done++) {
        if (!page_clone_row(dst, (CellCountInt)(dst_y + done), src,
                            (CellCountInt)(src_y + done))) {
            break;
        }
    }
    return done;
}

/* Erase a row and reset the flags that described its old contents.
 *
 * page_erase_row leaves the flags alone, because its callers go on to
 * overwrite them. A row scrolled into view is not overwritten — it is the
 * blank line the user now sees — so a stale wrap flag there would join it to
 * a line it has nothing to do with. */
inline void page_clear_row(Page *p, CellCountInt y) {
    if (!p || y >= p->capacity.rows) return;

    page_erase_row(p, y);

    Row *row = p->get_row(y);
    const Offset<Cell> cells_off = row->cells();
    *row = Row();
    row->set_cells(cells_off);
    row->set_dirty(true);
}

/* ─── resize and reflow ──────────────────────────────────────────────────── */

/* True if a cell holds nothing worth keeping.
 *
 * Trailing cells like this are dropped when a soft-wrapped line is re-laid,
 * since they describe where the old width happened to fall rather than
 * anything the program wrote. A cell holding a space is not blank: the
 * program asked for that space, and erasing it would change a line that used
 * spaces for alignment. */
inline bool page_cell_is_blank(const Cell *c) {
    return !c->has_text() && !c->has_bg_color() && !c->hyperlink() &&
           c->style_id() == style::DEFAULT_ID && c->wide() == Wide::narrow;
}

/* How much of a row is content.
 *
 * A soft-wrapped row is normally full — it wrapped because the text ran off
 * the end — so trimming it usually finds nothing. Trimming anyway costs a
 * scan of an already-full row and means a row carrying a wrap flag it should
 * not have does not drag a tail of blanks through the reflow. */
inline CellCountInt page_row_used_width(Page *p, CellCountInt y) {
    Cell *cells = p->get_cells(y);
    CellCountInt w = p->capacity.cols;
    while (w > 0 && page_cell_is_blank(&cells[w - 1])) w--;
    return w;
}

/* Told where each cell went.
 *
 * Reflow moves a cell to a row and column that have nothing to do with the
 * ones it came from, so anything remembering a position — a cursor, the ends
 * of a selection, a viewport someone scrolled to — is wrong the moment a
 * resize finishes. Recomputing those afterwards would mean guessing. The
 * reflow already knows the answer as it goes, so it says so, and the caller
 * writes it down.
 *
 * This is deliberately a callback rather than a list of positions to fix up:
 * page.hpp knows nothing about lists, cursors or selections, and this keeps
 * it that way. */
struct ReflowEvent {
    CellCountInt src_y;
    CellCountInt src_x;
    CellCountInt dst_y;
    CellCountInt dst_x;

    /* Fired once per source row after its last cell, with src_x set to the
     * row's used width. A position in a row's trailing blanks — which is
     * where a cursor usually sits — is never carried by a cell, so this is
     * the only thing that can place it. */
    bool at_row_end;
};

typedef void (*ReflowObserver)(void *ctx, const ReflowEvent &e);

/* Where a reflow has got to in the destination.
 *
 * Reflow is resumable, and this is what makes it so. A logical line can be
 * longer than either page involved — it can begin in one source page and end
 * in another, and it can fill a destination page and continue on the next —
 * so neither side's page boundary may be allowed to end a line. The cursor is
 * the state that survives a page boundary: which destination row and column
 * come next, and whether a line is currently open. */
struct ReflowCursor {
    CellCountInt y;   /* the destination row being written */
    CellCountInt x;   /* the next column in it */

    /* Row y has been cleared and had its flags set. */
    bool row_begun;

    /* A logical line is open: the next cell continues it rather than starting
     * a new one. This is what keeps a line whole across a page boundary. */
    bool in_line;

    ReflowCursor() : y(0), x(0), row_begun(false), in_line(false) {}
    explicit ReflowCursor(CellCountInt start_y)
        : y(start_y), x(0), row_begun(false), in_line(false) {}
};

/* What a reflow did, and where it stopped.
 *
 * The stopping point is a row *and a column*, because the destination can
 * fill in the middle of a source row. Resuming from the row alone would write
 * its first half a second time. */
struct ReflowResult {
    CellCountInt rows_written;       /* destination rows begun */
    CellCountInt src_rows_consumed;  /* source rows finished */

    CellCountInt src_y;   /* resume here */
    CellCountInt src_x;

    /* The destination ran out of rows. The caller supplies another page and
     * calls again with the same cursor. */
    bool full;

    /* False if a cell could not be copied — the destination's style, link or
     * grapheme storage filled up. What was written before that stands. */
    bool ok;

    ReflowResult()
        : rows_written(0), src_rows_consumed(0), src_y(0), src_x(0),
          full(false), ok(true) {}
};

/* Open a destination row: clear it, and say whether it continues a line. */
inline void page_reflow_begin_row(Page *dst, CellCountInt y, bool continuation) {
    page_clear_row(dst, y);
    dst->get_row(y)->set_wrap_continuation(continuation);
}

/* Re-lay source rows at the destination's width, continuing from a cursor.
 *
 * This is the resize that matters. A terminal's rows are not independent: a
 * line too long for the screen is stored as several rows joined by the wrap
 * flag, and changing the width means taking those runs apart and laying them
 * out again. Rows the program hard-ended with a newline are left alone, which
 * is the whole purpose of distinguishing a soft wrap from a hard one.
 *
 * Nothing looks ahead for where a logical line ends, because nothing needs
 * to: a row whose wrap flag is set simply does not close the destination row
 * it was writing into, and the next source row — in this page or in the next
 * one the caller passes — carries on where it left off. That is also what
 * makes the operation resumable at all.
 *
 * Reflow works in cells rather than rows because a cell rarely lands on the
 * column it came from. Wide characters make that concrete: one will not
 * straddle the new right edge, so if a single column is left the destination
 * gets a spacer_head there and the pair moves to the next row. Reading the
 * source, spacer_head cells are dropped — they record where the *old* width
 * fell and say nothing about the text. */
inline ReflowResult page_reflow_resume(Page *dst, ReflowCursor *cur, Page *src,
                                       CellCountInt src_y, CellCountInt src_x,
                                       CellCountInt src_rows,
                                       ReflowObserver obs = nullptr,
                                       void *obs_ctx = nullptr) {
    ReflowResult r;
    r.src_y = src_y;
    r.src_x = src_x;

    if (!dst || !src || !cur || dst->capacity.cols == 0) {
        r.ok = false;
        return r;
    }

    const CellCountInt dst_cols = dst->capacity.cols;
    const CellCountInt dst_rows = dst->capacity.rows;

    CellCountInt src_end = (CellCountInt)(src_y + src_rows);
    if (src_end > src->capacity.rows) src_end = src->capacity.rows;

    for (CellCountInt y = src_y; y < src_end; y++) {
        const CellCountInt width = page_row_used_width(src, y);
        Cell *src_cells = src->get_cells(y);

        CellCountInt x = (y == src_y) ? src_x : 0;
        while (x < width) {
            Cell *sc = &src_cells[x];

            /* Artifacts of the old width, not content. A tail is emitted with
             * its lead below, so it is skipped here too. */
            if (sc->wide() == Wide::spacer_head ||
                sc->wide() == Wide::spacer_tail) {
                x++;
                continue;
            }

            const CellCountInt units = sc->wide() == Wide::wide ? 2 : 1;

            /* A destination too narrow to hold a wide character at all. There
             * is nowhere to put it, and trying would not terminate. */
            if (units > dst_cols) {
                x++;
                continue;
            }

            if (cur->row_begun && (CellCountInt)(cur->x + units) > dst_cols) {
                /* A wide character will not be split across the edge. The
                 * leftover column gets a spacer_head, which is exactly what
                 * that state is for. */
                if (units == 2 && cur->x < dst_cols) {
                    Cell *pad = dst->get_cell(cur->x, cur->y);
                    *pad = Cell();
                    pad->set_wide(Wide::spacer_head);
                }
                dst->get_row(cur->y)->set_wrap(true);
                cur->y++;
                cur->x = 0;
                cur->row_begun = false;
            }

            if (!cur->row_begun) {
                if (cur->y >= dst_rows) {
                    r.full = true;
                    r.src_y = y;
                    r.src_x = x;
                    return r;
                }
                page_reflow_begin_row(dst, cur->y, cur->in_line);
                if (!cur->in_line) {
                    /* Prompt marks belong to the line, so they follow its
                     * first row rather than being smeared across the rows the
                     * new width produced. */
                    dst->get_row(cur->y)->set_semantic_prompt(
                        src->get_row(y)->semantic_prompt());
                }
                cur->row_begun = true;
                cur->in_line = true;
                r.rows_written++;
            }

            if (obs) {
                ReflowEvent e;
                e.src_y = y;
                e.src_x = x;
                e.dst_y = cur->y;
                e.dst_x = cur->x;
                e.at_row_end = false;
                obs(obs_ctx, e);
            }

            if (!page_clone_cell(dst, cur->x, cur->y, src, x, y)) {
                r.ok = false;
                r.src_y = y;
                r.src_x = x;
                return r;
            }
            cur->x++;
            x++;

            /* A wide character's tail travels with it, so the pair is never
             * separated by the move. */
            if (units == 2) {
                if (x < width && src_cells[x].wide() == Wide::spacer_tail) {
                    if (!page_clone_cell(dst, cur->x, cur->y, src, x, y)) {
                        r.ok = false;
                        r.src_y = y;
                        r.src_x = x;
                        return r;
                    }
                    x++;
                }
                cur->x++;
            }
        }

        if (obs) {
            ReflowEvent e;
            e.src_y = y;
            e.src_x = width;
            e.dst_y = cur->y;
            e.dst_x = cur->x;
            e.at_row_end = true;
            obs(obs_ctx, e);
        }

        /* A row whose wrap flag is set does not end its line, so the
         * destination row it was filling stays open for what comes next. */
        if (!src->get_row(y)->wrap()) {
            if (!cur->row_begun) {
                /* A blank hard-ended line still occupies a row. Dropping it
                 * would close up gaps a program deliberately left. */
                if (cur->y >= dst_rows) {
                    r.full = true;
                    r.src_y = y;
                    r.src_x = 0;
                    return r;
                }
                page_reflow_begin_row(dst, cur->y, false);
                dst->get_row(cur->y)->set_semantic_prompt(
                    src->get_row(y)->semantic_prompt());
                r.rows_written++;
            }

            dst->get_row(cur->y)->set_wrap(false);
            cur->y++;
            cur->x = 0;
            cur->row_begun = false;
            cur->in_line = false;
        }

        r.src_rows_consumed++;
        r.src_y = (CellCountInt)(y + 1);
        r.src_x = 0;
    }

    return r;
}

/* Re-lay rows into a destination starting at dst_y, in a single call.
 *
 * The common case, and what the per-page tests use. A caller that needs to
 * carry a line across a page boundary wants page_reflow_resume. */
inline ReflowResult page_reflow_into(Page *dst, CellCountInt dst_y, Page *src,
                                     CellCountInt src_y, CellCountInt src_rows) {
    ReflowCursor cur(dst_y);
    return page_reflow_resume(dst, &cur, src, src_y, 0, src_rows);
}

/* ─── scrolling ──────────────────────────────────────────────────────────── */

/* Scrolling moves rows, and a row is a handle rather than a container.
 *
 * A Row is 64 bits: some flags and the offset of its cells. Moving a row up
 * the screen is therefore a swap of two 64-bit values, not a copy of a
 * screenful of cells — scrolling a 200-column region costs the same as
 * scrolling a 2000-column one.
 *
 * This works because everything hanging off a row is keyed by the *cell's*
 * offset, not by its coordinates. A grapheme run and a hyperlink are found
 * through the page's maps using the offset of the cell that owns them, and
 * that offset travels with the row's cells pointer. Scrolling therefore needs
 * no fixups at all: the maps are still right the instant the swap is done.
 * Had a cell's extras been keyed by row and column, every scroll would have
 * meant rebuilding both maps.
 */

/* Exchange two rows. Their cells stay exactly where they are; only which row
 * points at them changes. */
inline void page_swap_rows(Page *p, CellCountInt a, CellCountInt b) {
    if (!p || a == b) return;
    if (a >= p->capacity.rows || b >= p->capacity.rows) return;

    Row *ra = p->get_row(a);
    Row *rb = p->get_row(b);

    const Row tmp = *ra;
    *ra = *rb;
    *rb = tmp;

    ra->set_dirty(true);
    rb->set_dirty(true);
}

/* Reverse a run of rows in place. Rotation is three reversals, which needs no
 * scratch space for the rows being displaced — worth having in a header that
 * cannot allocate. */
inline void page_reverse_rows(Page *p, CellCountInt lo, CellCountInt hi) {
    while (lo < hi) {
        page_swap_rows(p, lo, hi);
        lo++;
        hi--;
    }
}

/* Scroll a region up by n rows: the top n rows leave, and n blank rows arrive
 * at the bottom.
 *
 * top and bot are both inclusive, matching the DECSTBM scroll region the
 * caller will have parsed. */
inline void page_scroll_up(Page *p, CellCountInt top, CellCountInt bot,
                           CellCountInt n) {
    if (!p || n == 0) return;
    if (bot >= p->capacity.rows) bot = (CellCountInt)(p->capacity.rows - 1);
    if (top > bot) return;

    const CellCountInt count = (CellCountInt)(bot - top + 1);

    /* Scrolling by at least the region's height leaves nothing behind. */
    if (n >= count) {
        for (CellCountInt y = top; y <= bot; y++) page_clear_row(p, y);
        return;
    }

    /* The departing rows are released here, while they are still findable.
     * After the rotation they are the blank rows at the bottom. */
    for (CellCountInt i = 0; i < n; i++) {
        page_clear_row(p, (CellCountInt)(top + i));
    }

    page_reverse_rows(p, top, (CellCountInt)(top + n - 1));
    page_reverse_rows(p, (CellCountInt)(top + n), bot);
    page_reverse_rows(p, top, bot);
}

/* Scroll a region down by n rows: the bottom n rows leave, and n blank rows
 * arrive at the top. */
inline void page_scroll_down(Page *p, CellCountInt top, CellCountInt bot,
                             CellCountInt n) {
    if (!p || n == 0) return;
    if (bot >= p->capacity.rows) bot = (CellCountInt)(p->capacity.rows - 1);
    if (top > bot) return;

    const CellCountInt count = (CellCountInt)(bot - top + 1);

    if (n >= count) {
        for (CellCountInt y = top; y <= bot; y++) page_clear_row(p, y);
        return;
    }

    for (CellCountInt i = 0; i < n; i++) {
        page_clear_row(p, (CellCountInt)(bot - i));
    }

    /* The same rotation the other way round. */
    page_reverse_rows(p, top, bot);
    page_reverse_rows(p, top, (CellCountInt)(top + n - 1));
    page_reverse_rows(p, (CellCountInt)(top + n), bot);
}

/* ─── closing the style/page cycle ───────────────────────────────────────── */

/* These were declared in style.hpp against a forward-declared Cell. Now that
 * Cell is complete they can be defined. */

inline bool style::Style::bg(const Cell *cell, const Palette *palette, RGB *out) const {
    /* A cell's own background wins over the style's, which is what makes
     * background-only cells able to skip the style map entirely. */
    if (cell) {
        switch (cell->content_tag()) {
            case ContentTag::bg_color_palette:
                if (!palette) return false;
                *out = (*palette)[cell->color_palette()];
                return true;
            case ContentTag::bg_color_rgb:
                *out = cell->color_rgb();
                return true;
            default:
                break;
        }
    }

    switch (bg_color.tag) {
        case StyleColor::Tag::none:
            return false;
        case StyleColor::Tag::palette:
            if (!palette) return false;
            *out = (*palette)[bg_color.palette];
            return true;
        case StyleColor::Tag::rgb:
            *out = bg_color.rgb;
            return true;
    }
    return false;
}

inline bool style::Style::bg_cell(Cell *out) const {
    switch (bg_color.tag) {
        case StyleColor::Tag::none:
            return false;
        case StyleColor::Tag::palette:
            out->set_content_tag(ContentTag::bg_color_palette);
            out->set_color_palette(bg_color.palette);
            return true;
        case StyleColor::Tag::rgb:
            out->set_content_tag(ContentTag::bg_color_rgb);
            out->set_color_rgb(bg_color.rgb);
            return true;
    }
    return false;
}

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_PAGE_HPP */
