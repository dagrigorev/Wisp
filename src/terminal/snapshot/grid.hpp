/* Transliterated from Ghostty src/terminal/snapshot/grid.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Grid (rows, cells, and grapheme suffixes) encoding.
 *
 * A grid contains the rows and cells of a terminal page. Its dimensions are
 * supplied by the containing record rather than repeated here. The encoder
 * writes exactly `rows` row records followed by one grapheme suffix section.
 *
 * All records are tightly packed with no padding between them. All integers
 * are unsigned and little-endian.
 *
 * The layout is designed so the common case decodes with bulk copies: cells
 * are fixed-size words, trailing default cells are elided per row, and the
 * variable-length grapheme suffixes live outside the row data.
 *
 * ## Row
 *
 * | Offset | Size              | Field                      |
 * | -----: | ----------------: | :------------------------- |
 * |      0 |                 1 | Row flags                  |
 * |      1 |                 2 | Encoded cell count (`u16`) |
 * |      3 | `width` * `count` | Encoded cells              |
 *
 * Row flag bits: 0 wrap, 1 wrap continuation, 2-3 semantic prompt (0 none,
 * 1 prompt, 2 prompt continuation; 3 decodes as none), 4-5 encoded cell
 * width, 6-7 reserved zero.
 *
 * The encoded cell count must not exceed the grid's column count; it is a
 * structural field and decoders reject larger values. Cells at and beyond
 * the count are the default cell: all sixty-four bits zero. Canonical
 * encoders emit exactly through the row's last non-default cell.
 *
 * ## Encoded cell width
 *
 * `1 << width` bytes encode each of the row's cells. Narrower widths are
 * truncated transports of the same cell word: a cell qualifies for a width
 * when all of its higher word bits are zero.
 *
 * | Width | Bytes | Encoded value and admitted cells                      |
 * | ----: | ----: | :---------------------------------------------------- |
 * |     0 |     1 | Codepoint at or below U+00FF; all other bits zero     |
 * |     1 |     2 | Codepoint at or below U+FFFF; all other bits zero     |
 * |     2 |     4 | Word bits 0-31: any content kind and codepoint, style |
 * |       |       | IDs 1-63, narrow, no flags, no hyperlink              |
 * |     3 |     8 | The complete word                                     |
 *
 * Widths zero and one store the codepoint value itself (word bits 2-25
 * shifted down). Width two stores the word's low half unshifted. Canonical
 * encoders choose each row's smallest admissible width; decoders accept
 * rows encoded wider than necessary.
 *
 * Native row cache flags are not encoded. The Kitty virtual placeholder
 * hint is derived while decoding cells containing U+10EEEE.
 *
 * ## Cell
 *
 * One 64-bit little-endian word: kind 0-1, content 2-25, style ID 26-41,
 * width kind 42-43, protected 44, hyperlink flag 45, semantic content
 * 46-47, hyperlink ID 48-63.
 *
 * Content kinds: 0 codepoint, 1 codepoint with grapheme suffix, 2 palette
 * background (low content byte; rest reserved and ignored), 3 RGB
 * background (r, g, b bytes). Codepoints above U+10FFFF and surrogates
 * decode as U+FFFD. Width kinds: 0 narrow, 1 wide, 2 spacer tail, 3 spacer
 * head. Semantic content: 0 output, 1 input, 2 prompt; 3 decodes as output.
 *
 * Style and hyperlink ID zero mean none. Other IDs refer to the containing
 * record's tables. The hyperlink flag is set exactly when the hyperlink ID
 * is nonzero; decoders derive linkage from the remapped ID and ignore it.
 *
 * ## Grapheme suffix section
 *
 * A `u32` entry count, then entries of: row `u16`, column `u16`, codepoint
 * count `u16`, codepoints (`u32` each). Invalid scalars are individually
 * ignored. Canonical encoders emit one entry per kind 1 cell in ascending
 * row-then-column order. Decoders consume every entry and ignore those
 * whose target is out of range, not a codepoint cell, a zero codepoint, or
 * already received an entry.
 *
 * Wisp: the full upstream format text is abridged above; grid.zig is the
 * authority. Differences in shape rather than behavior:
 *   - `@Vector` SIMD in the bulk codec becomes scalar loops over the same
 *     words, transporting the same bytes.
 *   - The port's readers expose no `buffered()`/`toss()`, so decode always
 *     takes upstream's own streaming fallback for each path.
 *   - The writer is `std::string *`, which always has capacity, so the
 *     encoder's spare-capacity fast path and its writeAll fallback emit
 *     the same bytes through `append`.
 *   - Packed structs `Row` and `Cell` become plain structs with `bits()` /
 *     `fromBits()` performing the exact `@bitCast`.
 *   - `std.DynamicBitSetUnmanaged` in `Remap` is an allocator-owned bitmap.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_GRID_HPP
#define WISP_TERMINAL_SNAPSHOT_GRID_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../vt/page.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/reader.hpp"
#include "io.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace grid {

namespace terminal_page = ::wisp::vt::page;
namespace terminal_hyperlink = ::wisp::vt::hyperlink;
namespace terminal_style = ::wisp::vt::style;

typedef ::wisp::zigstd::ReadError ReadError;

/* The Kitty virtual-placement placeholder remains wire-relevant even when
 * runtime Kitty graphics support is compiled out. */
static const uint32_t kitty_virtual_placeholder = 0x10EEEE;

typedef terminal_page::Cell TerminalCell;
typedef terminal_hyperlink::Id TerminalHyperlinkId;
typedef terminal_page::Page TerminalPage;
typedef terminal_page::Row TerminalRow;
typedef terminal_style::Id TerminalStyleId;

/* The encoded cell width declared by a row header: how many bytes
 * transport each of the row's cell words.
 *
 * `truncate` and `extend` are the transport transform itself, so the
 * admission masks derive from them rather than being maintained by
 * hand: a cell word is admitted exactly when it round-trips. */
enum class EncodedWidth : uint8_t {
    one = 0,
    two = 1,
    four = 2,
    eight = 3,
};

/* The wire layout of one encoded cell. This is its own registry: the bit
 * positions and field meanings are part of the snapshot format and are
 * documented above independently of the native cell.
 *
 * Enum-like fields are raw integers because decoders must accept reserved
 * values (for example semantic content 3) without instantiating an invalid
 * native enum. */
struct Cell {
    uint8_t kind = 0;          /* u2 */
    uint32_t content = 0;      /* u24 */
    uint16_t style_id = 0;
    uint8_t width = 0;         /* u2 */
    bool protected_ = false;
    bool hyperlink = false;
    uint8_t semantic_content = 0; /* u2 */
    uint16_t hyperlink_id = 0;

    /* Determines how `content` is interpreted. */
    enum class Kind : uint8_t {
        codepoint = 0,
        codepoint_grapheme = 1,
        bg_color_palette = 2,
        bg_color_rgb = 3,
    };

    /* Wisp: @bitOffsetOf for each field. */
    static const unsigned kind_off = 0;
    static const unsigned content_off = 2;
    static const unsigned style_id_off = 26;
    static const unsigned width_off = 42;
    static const unsigned protected_off = 44;
    static const unsigned hyperlink_off = 45;
    static const unsigned semantic_content_off = 46;
    static const unsigned hyperlink_id_off = 48;

    /* Wisp: @bitCast(u64). */
    uint64_t bits() const {
        return ((uint64_t)(kind & 0x3) << kind_off) |
               ((uint64_t)(content & 0xFFFFFF) << content_off) |
               ((uint64_t)style_id << style_id_off) |
               ((uint64_t)(width & 0x3) << width_off) |
               ((uint64_t)(protected_ ? 1 : 0) << protected_off) |
               ((uint64_t)(hyperlink ? 1 : 0) << hyperlink_off) |
               ((uint64_t)(semantic_content & 0x3) << semantic_content_off) |
               ((uint64_t)hyperlink_id << hyperlink_id_off);
    }

    /* Wisp: @bitCast(Cell, u64). */
    static Cell fromBits(uint64_t v) {
        Cell c;
        c.kind = (uint8_t)((v >> kind_off) & 0x3);
        c.content = (uint32_t)((v >> content_off) & 0xFFFFFF);
        c.style_id = (uint16_t)((v >> style_id_off) & 0xFFFF);
        c.width = (uint8_t)((v >> width_off) & 0x3);
        c.protected_ = ((v >> protected_off) & 1) != 0;
        c.hyperlink = ((v >> hyperlink_off) & 1) != 0;
        c.semantic_content = (uint8_t)((v >> semantic_content_off) & 0x3);
        c.hyperlink_id = (uint16_t)((v >> hyperlink_id_off) & 0xFFFF);
        return c;
    }
};

/* The number of bytes transporting one cell word. */
inline size_t widthSize(EncodedWidth self) {
    return (size_t)1 << (unsigned)self;
}

/* Truncate one cell word to its transported value. Only words admitted by
 * this width round-trip; `select` proves that for every cell in a row
 * before an encoder may use it.
 *
 * Wisp: `self.Int()` is carried as a u64 holding the truncated value. */
inline uint64_t widthTruncate(EncodedWidth self, uint64_t word) {
    switch (self) {
        /* The bare codepoint, shifted down from the content field. */
        case EncodedWidth::one: return Cell::fromBits(word).content & 0xFF;
        case EncodedWidth::two: return Cell::fromBits(word).content & 0xFFFF;

        /* The word itself. */
        case EncodedWidth::four: return word & 0xFFFFFFFFull;
        case EncodedWidth::eight: return word;
    }
    return word;
}

/* Widen one transported value back to its cell word. */
inline uint64_t widthExtend(EncodedWidth self, uint64_t value) {
    switch (self) {
        case EncodedWidth::one:
        case EncodedWidth::two: {
            Cell c;
            c.content = (uint32_t)value;
            return c.bits();
        }
        case EncodedWidth::four:
        case EncodedWidth::eight:
            return value;
    }
    return value;
}

/* Wisp: std.math.maxInt(self.Int()). */
inline uint64_t widthIntMax(EncodedWidth self) {
    switch (self) {
        case EncodedWidth::one: return 0xFF;
        case EncodedWidth::two: return 0xFFFF;
        case EncodedWidth::four: return 0xFFFFFFFFull;
        case EncodedWidth::eight: return ~(uint64_t)0;
    }
    return ~(uint64_t)0;
}

/* The word bits a cell may use and still round-trip through this width. */
inline uint64_t widthMask(EncodedWidth self) {
    return widthExtend(self, widthIntMax(self));
}

/* The smallest width admitting the word, typically the bitwise OR of every
 * cell word in a row. */
inline EncodedWidth widthSelect(uint64_t word) {
    const EncodedWidth widths[] = { EncodedWidth::one, EncodedWidth::two, EncodedWidth::four };
    for (size_t i = 0; i < 3; i++) {
        if ((word & ~widthMask(widths[i])) == 0) return widths[i];
    }
    return EncodedWidth::eight;
}

/* The header before every row's encoded cells.
 *
 * The semantic prompt is a raw integer for the same reason as the wire
 * cell fields: decoders must accept its reserved value without
 * instantiating an invalid native enum. The width enum is exhaustive over
 * its two bits, so every header byte bit-casts to a valid value. */
struct Row {
    bool wrap = false;
    bool wrap_continuation = false;
    uint8_t semantic_prompt = 0; /* u2 */
    EncodedWidth cell_width = EncodedWidth::one;
    uint8_t padding_ = 0; /* u2 */

    uint8_t bits() const {
        return (uint8_t)((wrap ? 1 : 0) | (wrap_continuation ? 2 : 0) |
                         ((semantic_prompt & 0x3) << 2) |
                         (((uint8_t)cell_width & 0x3) << 4) |
                         ((padding_ & 0x3) << 6));
    }

    static Row fromBits(uint8_t v) {
        Row r;
        r.wrap = (v & 1) != 0;
        r.wrap_continuation = (v & 2) != 0;
        r.semantic_prompt = (uint8_t)((v >> 2) & 0x3);
        r.cell_width = (EncodedWidth)((v >> 4) & 0x3);
        r.padding_ = (uint8_t)((v >> 6) & 0x3);
        return r;
    }
};

/* Whether the native cell's in-memory layout matches the wire cell layout
 * bit for bit, with the wire hyperlink ID occupying the native padding.
 *
 * The wire format does not require this: it is an optimization. When it
 * holds, whole rows encode and decode as bulk copies. If the native layout
 * ever diverges, the portable field-by-field codec below remains correct
 * and this simply becomes false.
 *
 * Each probe pairs a native cell with the wire cell that must share its
 * exact bit pattern; any difference means some field moved or some enum
 * member was renumbered. Three probes, one per content payload, cover the
 * whole cell.
 *
 * Wisp: computed once at first use rather than at comptime. */
inline bool nativeMatchesWire() {
    static const bool result = []() -> bool {
        if (sizeof(TerminalCell) * 8 != 64) return false;

        TerminalCell n0;
        n0.setContentTag(TerminalCell::ContentTag::codepoint_grapheme);
        n0.setContentCodepoint(0x10FFFF);
        n0.setStyleId(0xBEEF);
        n0.setWide(TerminalCell::Wide::spacer_tail);
        n0.setProtected(true);
        n0.bits |= (uint64_t)0x1D2C << 48; /* ._padding = 0x1D2C */
        Cell w0;
        w0.kind = 1;
        w0.content = 0x10FFFF;
        w0.style_id = 0xBEEF;
        w0.width = 2;
        w0.protected_ = true;
        /* The native padding position, canonical wire or not. */
        w0.hyperlink_id = 0x1D2C;

        TerminalCell n1;
        n1.setContentTag(TerminalCell::ContentTag::bg_color_palette);
        n1.setContentColorPalette(0xAB);
        n1.setWide(TerminalCell::Wide::spacer_head);
        n1.setHyperlink(true);
        n1.setSemanticContent(TerminalCell::SemanticContent::input);
        Cell w1;
        w1.kind = 2;
        w1.content = 0xAB;
        w1.width = 3;
        w1.hyperlink = true;
        w1.semantic_content = 1;

        TerminalCell n2;
        n2.setContentTag(TerminalCell::ContentTag::bg_color_rgb);
        TerminalCell::RGB rgb;
        rgb.r = 0x12;
        rgb.g = 0x34;
        rgb.b = 0x56;
        n2.setContentColorRgb(rgb);
        n2.setWide(TerminalCell::Wide::wide);
        n2.setSemanticContent(TerminalCell::SemanticContent::prompt);
        Cell w2;
        w2.kind = 3;
        w2.content = 0x563412;
        w2.width = 1;
        w2.semantic_content = 2;

        if (n0.bits != w0.bits()) return false;
        if (n1.bits != w1.bits()) return false;
        if (n2.bits != w2.bits()) return false;
        return true;
    }();
    return result;
}

/* Whether rows of cells can be copied between native and wire storage
 * without per-cell transformation.
 *
 * Wisp: x64 is little-endian, the only target. */
inline bool bulkCodec() { return nativeMatchesWire(); }

enum class EncodeError : uint8_t {
    none = 0,

    /* Wide and spacer cells do not form a valid row. */
    InvalidWideCell,

    /* One cell's grapheme suffix exceeds the entry's u16 codepoint count. */
    TooManyGraphemes,
};

enum class DecodeError : uint8_t {
    none = 0,

    /* std.Io.Reader.Error */
    EndOfStream,

    /* A row declares more encoded cells than the grid has columns. */
    InvalidRowCellCount,
};

/* Maps encoded table IDs to IDs assigned by the destination page.
 *
 * Build this by inserting each decoded table entry into the page, then
 * recording the encoded ID and the ID returned by the page's set. ID zero
 * is implicit and does not need an entry. Lookup must be cheap because the
 * grid decoder consults it for every styled or linked cell, so this is a
 * direct-indexed table rather than a hash map. */
template <typename Id>
struct Remap {
    /* One slot for every possible encoded ID. */
    static const size_t capacity = (size_t)(Id)~(Id)0 + 1;

    /* Indexed by encoded ID; zero means unmapped or default. */
    Id *entries = nullptr;
    size_t entries_len = 0;

    /* Tracks IDs that received an entry, including ones mapped to the
     * default, so callers can give duplicate table entries first-wins
     * semantics. Wisp: one bit per ID, allocator-owned. */
    uint8_t *seen = nullptr;

    /* A remap with no entries at all: every lookup is unmapped. Use this
     * instead of `init` when the encoded table is empty so pages without
     * styles or hyperlinks allocate nothing. */
    static Remap empty() { return Remap(); }

    /* Wisp: false is OutOfMemory. */
    static bool init(::wisp::zigstd::Allocator alloc, Remap *out) {
        Id *entries = alloc.allocT<Id>(capacity);
        if (entries == nullptr) return false;
        memset(entries, 0, capacity * sizeof(Id));
        uint8_t *seen = alloc.alloc(capacity / 8);
        if (seen == nullptr) {
            alloc.freeT(entries, capacity); /* errdefer */
            return false;
        }
        memset(seen, 0, capacity / 8);
        out->entries = entries;
        out->entries_len = capacity;
        out->seen = seen;
        return true;
    }

    void deinit(::wisp::zigstd::Allocator alloc) {
        if (entries_len != 0) {
            alloc.freeT(entries, capacity);
            alloc.free(seen, capacity / 8);
        }
        entries = nullptr;
        entries_len = 0;
        seen = nullptr;
    }

    /* Record one encoded-to-native mapping. Illegal on `empty`. */
    void put(Id encoded, Id native) {
        assert(!contains(encoded));
        entries[encoded] = native;
        seen[encoded / 8] = (uint8_t)(seen[encoded / 8] | (1u << (encoded % 8)));
    }

    /* Whether the encoded ID already has an entry, even a default one.
     * Illegal on `empty`. */
    bool contains(Id encoded) const {
        return (seen[encoded / 8] & (1u << (encoded % 8))) != 0;
    }

    /* The native ID for an encoded ID, or zero when unmapped. */
    Id get(Id encoded) const {
        if (entries_len == 0) return 0;
        return entries[encoded];
    }
};

/* Maps encoded style table IDs to page-assigned style IDs. */
typedef Remap<TerminalStyleId> StyleRemap;

/* Maps encoded hyperlink table IDs to page-assigned hyperlink IDs. */
typedef Remap<TerminalHyperlinkId> HyperlinkRemap;

/* Whether the value is a valid Unicode scalar value. */
inline bool validScalar(uint32_t cp) {
    return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

/* The encoded word for one native cell and its hyperlink ID. */
inline uint64_t cellBits(const TerminalCell &cell, TerminalHyperlinkId link_id) {
    if (nativeMatchesWire()) {
        Cell wire = Cell::fromBits(cell.bits);
        wire.hyperlink = link_id != 0;
        wire.hyperlink_id = link_id;
        return wire.bits();
    }

    Cell wire;
    wire.kind = (uint8_t)cell.content_tag();
    switch (cell.content_tag()) {
        case TerminalCell::ContentTag::codepoint:
        case TerminalCell::ContentTag::codepoint_grapheme:
            wire.content = cell.contentCodepoint();
            break;
        case TerminalCell::ContentTag::bg_color_palette:
            wire.content = cell.contentColorPalette();
            break;
        case TerminalCell::ContentTag::bg_color_rgb: {
            const TerminalCell::RGB c = cell.contentColorRgb();
            wire.content = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16);
            break;
        }
    }
    wire.style_id = cell.style_id();
    switch (cell.wide()) {
        case TerminalCell::Wide::narrow: wire.width = 0; break;
        case TerminalCell::Wide::wide: wire.width = 1; break;
        case TerminalCell::Wide::spacer_tail: wire.width = 2; break;
        case TerminalCell::Wide::spacer_head: wire.width = 3; break;
    }
    wire.protected_ = cell.protected_();
    wire.hyperlink = link_id != 0;
    switch (cell.semantic_content()) {
        case TerminalCell::SemanticContent::output: wire.semantic_content = 0; break;
        case TerminalCell::SemanticContent::input: wire.semantic_content = 1; break;
        case TerminalCell::SemanticContent::prompt: wire.semantic_content = 2; break;
    }
    wire.hyperlink_id = link_id;
    return wire.bits();
}

/* Store one normalized, hyperlink-free wire word as a native cell. */
inline void storeCell(TerminalCell *cell, uint64_t bits) {
    if (nativeMatchesWire()) {
        cell->bits = bits;
        return;
    }

    const Cell wire = Cell::fromBits(bits);
    TerminalCell native = TerminalCell::init(0);
    switch ((Cell::Kind)wire.kind) {
        case Cell::Kind::codepoint:
        case Cell::Kind::codepoint_grapheme:
            native.setContentCodepoint(wire.content);
            break;
        case Cell::Kind::bg_color_palette:
            native.setContentTag(TerminalCell::ContentTag::bg_color_palette);
            native.setContentColorPalette((uint8_t)wire.content);
            break;
        case Cell::Kind::bg_color_rgb: {
            native.setContentTag(TerminalCell::ContentTag::bg_color_rgb);
            TerminalCell::RGB c;
            c.r = (uint8_t)wire.content;
            c.g = (uint8_t)(wire.content >> 8);
            c.b = (uint8_t)(wire.content >> 16);
            native.setContentColorRgb(c);
            break;
        }
    }
    native.setStyleId(wire.style_id);
    native.setWide((TerminalCell::Wide)wire.width);
    native.setProtected(wire.protected_);
    native.setSemanticContent((TerminalCell::SemanticContent)wire.semantic_content);
    *cell = native;
}

/* The word used to select a row's encoded cell width. This is the cell's
 * wire word with the hyperlink flag reflecting the native cell, so linked
 * cells and nonzero native padding disqualify every narrow width. */
inline uint64_t classifyWord(const TerminalCell *cell) {
    if (nativeMatchesWire()) return cell->bits;

    Cell wire = Cell::fromBits(cellBits(*cell, 0));
    wire.hyperlink = cell->hyperlink();
    return wire.bits();
}

/* The encoded cell count (through the last nonzero cell) and the bitwise
 * OR of every encoded cell word for one row.
 *
 * Wisp: upstream's bulk path counts and ORs four words at a time with
 * @Vector; this is the same scan over the same words, one at a time. */
inline void scanRow(const TerminalCell *cells, size_t cells_len,
                    size_t *count_out, uint64_t *word_or_out) {
    size_t count = cells_len;
    while (count > 0 && cells[count - 1].isZero()) count -= 1;
    uint64_t word_or = 0;
    for (size_t i = 0; i < count; i++) word_or |= classifyWord(&cells[i]);
    *count_out = count;
    *word_or_out = word_or;
}

inline void appendLe(std::string *writer, uint64_t value, size_t size) {
    for (size_t b = 0; b < size; b++) writer->push_back((char)(uint8_t)(value >> (8 * b)));
}

/* Write one row's cells truncated to the given encoded width.
 *
 * Every admitted cell round-trips exactly because the row's width
 * selection proved every cell word survives `truncate` then `extend`. */
inline void encodeNarrowCells(EncodedWidth width, const TerminalCell *cells,
                              size_t count, std::string *writer) {
    const size_t size = widthSize(width);
    for (size_t i = 0; i < count; i++) {
        appendLe(writer, widthTruncate(width, classifyWord(&cells[i])), size);
    }
}

/* Encode the grapheme suffix section for every kind 1 cell in the grid.
 *
 * Wisp: upstream batches entries into a 4 KiB local buffer to save writer
 * calls; appending to the string directly emits the same bytes. */
inline EncodeError encodeGraphemes(const TerminalPage *page, std::string *writer) {
    /* Every grapheme cell owns exactly one entry in the page's grapheme
     * map, so the section header comes straight from the page without a
     * counting pass over the grid. Rows without the native grapheme hint
     * contain no grapheme cells in any intact page. */
    const size_t entries = page->graphemeCount();
    io::writeInt<uint32_t>(writer, (uint32_t)entries);
    if (entries == 0) return EncodeError::none;

    size_t emitted = 0;
    const size_t rows = page->size.rows;
    for (size_t y = 0; y < rows; y++) {
        const TerminalRow *row = page->getRow(y);
        if (!row->grapheme()) continue;
        const TerminalCell *cells = page->getCells(row);
        const size_t cols = page->size.cols;
        for (size_t x = 0; x < cols; x++) {
            const TerminalCell *cell = &cells[x];
            if (!cell->hasGrapheme()) continue;
            size_t cps_len = 0;
            const uint32_t *cps = page->lookupGrapheme(cell, &cps_len);
            assert(cps != nullptr);
            if (cps_len > 0xFFFF) return EncodeError::TooManyGraphemes;
            emitted += 1;

            io::writeInt<uint16_t>(writer, (uint16_t)y);
            io::writeInt<uint16_t>(writer, (uint16_t)x);
            io::writeInt<uint16_t>(writer, (uint16_t)cps_len);
            for (size_t i = 0; i < cps_len; i++) io::writeInt<uint32_t>(writer, cps[i]);
        }
    }

    /* The declared count is trusted by decoders for framing, so the grid
     * must have produced exactly that many entries. */
    assert(emitted == entries);
    (void)emitted;
    return EncodeError::none;
}

/* Encode every row, cell, and grapheme suffix directly from a page.
 *
 * If an error is returned, partial data may have been written. If you
 * want transactional writing, the caller is responsible for using something
 * like a seekable stream and rolling back. */
inline EncodeError encode(const TerminalPage *page, std::string *writer) {
    struct IntegrityGuard {
        const TerminalPage *p;
        ~IntegrityGuard() { p->assertIntegrity(); }
    } defer_integrity = { page };

    const size_t rows = page->size.rows;
    for (size_t y = 0; y < rows; y++) {
        const TerminalRow *row = page->getRow(y);
        const TerminalCell *cells = page->getCells(row);
        const size_t cells_len = page->size.cols;

        /* Trailing default cells decode implicitly: wide/spacer pairs and
         * hyperlinked or styled cells are always nonzero, so eliding the
         * zero suffix never drops encoded state. The scan also accumulates
         * the OR of the row's cell words to select its encoded cell width. */
        size_t count = 0;
        uint64_t word_or = 0;
        scanRow(cells, cells_len, &count, &word_or);

        /* Validate the wide state of every encoded cell so we don't encode
         * corrupt data. The width bits of the OR word witness whether any
         * encoded cell is non-narrow at all; rows without them, the common
         * case, satisfy every pair rule vacuously. Trailing default cells
         * are narrow, so checking the encoded prefix against the full row
         * width covers every pair. */
        Cell wide_cell;
        wide_cell.width = 3;
        const uint64_t wide_mask = wide_cell.bits();
        if ((word_or & wide_mask) != 0) {
            for (size_t x = 0; x < count; x++) {
                switch (cells[x].wide()) {
                    case TerminalCell::Wide::narrow: break;
                    case TerminalCell::Wide::wide:
                        if (x + 1 == cells_len || cells[x + 1].wide() != TerminalCell::Wide::spacer_tail) {
                            return EncodeError::InvalidWideCell;
                        }
                        break;
                    case TerminalCell::Wide::spacer_tail:
                        if (x == 0 || cells[x - 1].wide() != TerminalCell::Wide::wide) {
                            return EncodeError::InvalidWideCell;
                        }
                        break;
                    case TerminalCell::Wide::spacer_head:
                        if (x + 1 != cells_len || !row->wrap()) {
                            return EncodeError::InvalidWideCell;
                        }
                        break;
                }
            }
        }

        /* Canonical rows use the smallest admissible width. */
        const EncodedWidth cell_width = widthSelect(word_or);

        Row row_header;
        row_header.wrap = row->wrap();
        row_header.wrap_continuation = row->wrap_continuation();
        row_header.semantic_prompt = (uint8_t)row->semantic_prompt();
        row_header.cell_width = cell_width;

        /* Row header: flags then the encoded cell count. */
        writer->push_back((char)row_header.bits());
        io::writeInt<uint16_t>(writer, (uint16_t)count);

        switch (cell_width) {
            case EncodedWidth::one:
            case EncodedWidth::two:
            case EncodedWidth::four:
                encodeNarrowCells(cell_width, cells, count, writer);
                break;

            case EncodedWidth::eight: {
                /* Hyperlink IDs live in a native side table, but we embed
                 * them in ours, so if we have any hyperlinks we need to
                 * fallback to the loop below. */
                if (bulkCodec()) {
                    const Cell witness = Cell::fromBits(word_or);
                    if (!witness.hyperlink && witness.hyperlink_id == 0) {
                        writer->append((const char *)cells, count * sizeof(TerminalCell));
                        break;
                    }
                }

                for (size_t x = 0; x < count; x++) {
                    const TerminalCell *cell = &cells[x];
                    TerminalHyperlinkId link_id = 0;
                    if (cell->hyperlink()) {
                        const bool found = page->lookupHyperlink(cell, &link_id);
                        assert(found); /* orelse unreachable */
                        (void)found;
                    }
                    io::writeInt<uint64_t>(writer, cellBits(*cell, link_id));
                }
                break;
            }
        }
    }

    return encodeGraphemes(page, writer);
}

/* Read and validate one narrow transported value. */
inline uint64_t widenValue(EncodedWidth width, const uint8_t *bytes) {
    const size_t size = widthSize(width);
    uint64_t value = 0;
    for (size_t b = 0; b < size; b++) value |= (uint64_t)bytes[b] << (8 * b);

    /* Width one cannot encode an invalid scalar. Width two admits
     * surrogates, which degrade exactly like their full-width form. */
    if (width != EncodedWidth::one) {
        if (!validScalar((uint32_t)value)) return 0xFFFD;
    }
    return value;
}

/* Store codepoint-valued encoded cells of the given width. */
inline void widenCells(EncodedWidth width, const uint8_t *bytes,
                       TerminalCell *cells, size_t cells_len) {
    const size_t size = widthSize(width);
    for (size_t i = 0; i < cells_len; i++) {
        storeCell(&cells[i], widthExtend(width, widenValue(width, bytes + i * size)));
    }
}

/* Decode one row of width-one or width-two cells.
 *
 * These widths admit only bare codepoints, so cells store directly with at
 * most Unicode scalar validation: no styles, hyperlinks, wide pairs, or
 * row hints are reachable and no normalization state is needed. */
template <typename ReaderT>
inline DecodeError decodeNarrowCells(EncodedWidth width, ReaderT *reader,
                                     TerminalCell *cells, size_t cells_len) {
    const size_t size = widthSize(width);

    /* Streaming sources fall back to bounded chunks. */
    uint8_t chunk[1024];
    size_t i = 0;
    while (i < cells_len) {
        const size_t remaining = cells_len - i;
        const size_t n = remaining < sizeof chunk / size ? remaining : sizeof chunk / size;
        ReadError rerr = ReadError::none;
        if (!reader->readSliceAll(chunk, n * size, &rerr)) return DecodeError::EndOfStream;
        widenCells(width, chunk, cells + i, n);
        i += n;
    }
    return DecodeError::none;
}

/* Normalize one encoded cell word and store it at `cells[x]`.
 *
 * This owns every per-cell decode rule except grapheme suffixes and
 * wide-pair resolution: content validation, reserved-value degradation,
 * and style and hyperlink remapping with reference counting. Callers run
 * `normalizeWideRow` over the stored row afterward. */
inline void applyCell(TerminalPage *page, TerminalRow *row, TerminalCell *cells,
                      size_t x, uint64_t bits_wire, const StyleRemap *style_remap,
                      const HyperlinkRemap *hyperlink_remap) {
    TerminalCell *cell = &cells[x];

    /* The default cell is the common case and needs no normalization,
     * reference counting, or table lookups. */
    if (bits_wire == 0) {
        storeCell(cell, 0);
        return;
    }

    /* Any word is a valid wire cell because its fields are raw integers,
     * so all normalization below is plain field access. */
    Cell wire = Cell::fromBits(bits_wire);

    /* Hyperlink linkage is derived from the remapped ID below. The stored
     * native cell starts unlinked either way. */
    const uint16_t link_encoded = wire.hyperlink_id;
    wire.hyperlink_id = 0;
    wire.hyperlink = false;

    switch ((Cell::Kind)wire.kind) {
        /* Kind 1 differs from 0 only by declaring a grapheme suffix section
         * entry, which reattaches through the native grapheme APIs later. */
        case Cell::Kind::codepoint:
        case Cell::Kind::codepoint_grapheme:
            wire.kind = (uint8_t)Cell::Kind::codepoint;
            if (!validScalar(wire.content)) wire.content = 0xFFFD;

            /* Kitty image and placement state is not part of this snapshot
             * version, but the placeholder is still a valid Unicode scalar.
             * Preserve it and derive the native row hint so later row
             * operations remain correct. */
            if (wire.content == kitty_virtual_placeholder) {
                row->setKittyVirtualPlaceholder(true);
            }
            break;

        /* Only the palette index is meaningful; the remaining content bits
         * are reserved and must not obscure it. */
        case Cell::Kind::bg_color_palette:
            wire.content = (uint8_t)wire.content;
            break;

        case Cell::Kind::bg_color_rgb:
            break;
    }

    /* Reserved semantic content degrades to plain output. */
    if (wire.semantic_content > (uint8_t)TerminalCell::SemanticContent::prompt) {
        wire.semantic_content = (uint8_t)TerminalCell::SemanticContent::output;
    }

    /* IDs belong to the encoded page. Translate them to IDs assigned by the
     * destination page before storing them on cells. The table owns one
     * reference and each decoded cell owns one additional reference. */
    if (wire.style_id != 0) {
        wire.style_id = style_remap->get(wire.style_id);
        if (wire.style_id != 0) {
            page->styles.use((const void *)page->memory, wire.style_id);
            row->setStyled(true);
        }
    }

    storeCell(cell, wire.bits());

    if (link_encoded != 0) {
        const TerminalHyperlinkId link_native = hyperlink_remap->get(link_encoded);
        if (link_native == 0) return;

        /* setHyperlink records the cell mapping but intentionally does not
         * increment the set's reference count. If its map is full, undo our
         * reference and leave this cell unlinked. */
        page->hyperlink_set.use((const void *)page->memory, link_native);
        if (page->setHyperlink(row, cell, link_native) != terminal_page::PageError::none) {
            page->hyperlink_set.release((const void *)page->memory, link_native);
        }
    }
}

/* Resolve wide-pair relationships for the cell at `x` against its already
 * normalized predecessors. */
inline void normalizeWide(const TerminalRow *row, TerminalCell *cells,
                          size_t cells_len, size_t x) {
    typedef TerminalCell::Wide Wide;

    /* A following cell resolves whether the previous wide marker owns a
     * tail. Normalize the current marker immediately when possible,
     * including a wide marker at the row end. */
    if (x > 0 && cells[x - 1].wide() == Wide::wide && cells[x].wide() != Wide::spacer_tail) {
        cells[x - 1].setWide(Wide::narrow);
    }
    switch (cells[x].wide()) {
        case Wide::narrow: break;

        /* A non-final wide marker remains pending until the next cell. */
        case Wide::wide:
            if (x + 1 == cells_len) cells[x].setWide(Wide::narrow);
            break;

        case Wide::spacer_tail:
            if (x == 0 || cells[x - 1].wide() != Wide::wide) cells[x].setWide(Wide::narrow);
            break;

        case Wide::spacer_head:
            if (x + 1 != cells_len || !row->wrap()) cells[x].setWide(Wide::narrow);
            break;
    }
}

/* Resolve wide-pair relationships for one decoded row.
 *
 * Cell decoding stores every cell unresolved, so this pass applies
 * `normalizeWide` in order, which is equivalent to interleaving it with
 * the stores. Rows whose word OR carries no wide bits are already
 * normalized: every cell is narrow. Width four and narrower transports
 * cannot encode wide bits at all, so those rows skip the check entirely. */
inline void normalizeWideRow(EncodedWidth width, const TerminalRow *row,
                             TerminalCell *cells, size_t cells_len,
                             size_t count, uint64_t row_or) {
    if (width != EncodedWidth::eight) return;
    Cell wide_cell;
    wide_cell.width = 3;
    if ((row_or & wide_cell.bits()) == 0) return;
    for (size_t x = 0; x < count; x++) normalizeWide(row, cells, cells_len, x);
}

/* Decode one row of width-four or fallback full-width cells through the
 * complete per-cell normalization path.
 *
 * Returns the bitwise OR of every decoded wire word so callers can gate
 * the trailing wide-pair resolution pass without a second scan. */
template <typename ReaderT>
inline DecodeError decodeWordCells(EncodedWidth width, TerminalPage *page,
                                   TerminalRow *row, TerminalCell *cells,
                                   size_t cells_len, size_t count, ReaderT *reader,
                                   const StyleRemap *style_remap,
                                   const HyperlinkRemap *hyperlink_remap,
                                   uint64_t *row_or_out) {
    const size_t size = widthSize(width);
    uint64_t row_or = 0;

    /* Streaming sources fall back to per-cell reads. */
    for (size_t x = 0; x < count; x++) {
        uint8_t raw[8];
        ReadError rerr = ReadError::none;
        if (!reader->readSliceAll(raw, size, &rerr)) return DecodeError::EndOfStream;
        uint64_t value = 0;
        for (size_t b = 0; b < size; b++) value |= (uint64_t)raw[b] << (8 * b);
        const uint64_t bits = widthExtend(width, value);
        row_or |= bits;
        applyCell(page, row, cells, x, bits, style_remap, hyperlink_remap);
    }
    normalizeWideRow(width, row, cells, cells_len, count, row_or);
    if (row_or_out != nullptr) *row_or_out = row_or;
    return DecodeError::none;
}

/* Attach one decoded suffix codepoint to its resolved target cell.
 *
 * If native capacity is exhausted, any prefix already attached is removed
 * so the cell never exposes a truncated cluster, and `accept` latches
 * false so the entry's remaining codepoints are consumed but dropped. */
inline void applyGraphemeSuffix(TerminalPage *page, TerminalRow *row,
                                TerminalCell *cell, bool *accept, uint32_t cp) {
    if (!*accept) return;
    if (cp == 0 || !validScalar(cp)) return;

    if (page->appendGrapheme(row, cell, cp) != terminal_page::PageError::none) {
        if (cell->hasGrapheme()) {
            page->clearGrapheme(cell);
            page->updateRowGraphemeFlag(row);
        }
        *accept = false;
    }
}

/* Decode the grapheme suffix section into already decoded cells. */
template <typename ReaderT>
inline DecodeError decodeGraphemes(TerminalPage *page, ReaderT *reader) {
    ReadError rerr = ReadError::none;
    uint32_t entries = 0;
    if (!io::readInt<uint32_t>(reader, &entries, &rerr)) return DecodeError::EndOfStream;

    for (uint32_t e = 0; e < entries; e++) {
        uint16_t y = 0, x = 0, cp_count = 0;
        if (!io::readInt<uint16_t>(reader, &y, &rerr) ||
            !io::readInt<uint16_t>(reader, &x, &rerr) ||
            !io::readInt<uint16_t>(reader, &cp_count, &rerr)) {
            return DecodeError::EndOfStream;
        }

        /* Resolve the target cell. Entries whose target cannot carry a
         * suffix are optional detail: their codepoints are consumed to
         * preserve framing and then dropped. */
        TerminalRow *target_row = nullptr;
        TerminalCell *target_cell = nullptr;
        if (y < page->size.rows && x < page->size.cols) {
            TerminalRow *row = page->getRow(y);
            TerminalCell *cell = &page->getCells(row)[x];

            /* Cell decoding stores every kind 1 cell as a plain codepoint,
             * so this also gives duplicate entries first-wins semantics. */
            if (cell->content_tag() == TerminalCell::ContentTag::codepoint &&
                cell->contentCodepoint() != 0) {
                target_row = row;
                target_cell = cell;
            }
        }

        /* Always consume every declared codepoint. Invalid scalars and NUL
         * are not meaningful grapheme suffix components and are ignored. A
         * dropped entry's codepoints are discarded in bulk. */
        if (target_cell == nullptr) {
            if (!reader->discardAll((size_t)cp_count * 4, &rerr)) return DecodeError::EndOfStream;
            continue;
        }

        bool accept = true;
        for (size_t index = 0; index < cp_count; index++) {
            uint32_t cp = 0;
            if (!io::readInt<uint32_t>(reader, &cp, &rerr)) return DecodeError::EndOfStream;
            applyGraphemeSuffix(page, target_row, target_cell, &accept, cp);
        }
    }
    return DecodeError::none;
}

/* Decode every row, cell, and grapheme suffix directly into an
 * initialized, empty page.
 *
 * The grid does not encode dimensions, so `page` must already have the
 * exact row and column count expected by the containing record. Capacity
 * hints are advisory. Graphemes and cell hyperlink references that do not
 * fit are discarded without affecting the rest of the grid.
 *
 * Style and hyperlink table entries must be inserted into `page` before
 * calling this function, with their encoded and page-assigned IDs recorded
 * in `style_remap` and `hyperlink_remap`. ID zero always means the default
 * style or no hyperlink. A nonzero ID missing from its remap is also
 * treated as zero so unknown table references do not prevent the rest of
 * the grid from decoding.
 *
 * Invalid semantic data is normalized into a degraded form while
 * preserving the declared byte boundaries. Unknown semantic values use
 * their neutral variants, invalid Unicode becomes U+FFFD, invalid optional
 * data is ignored, and malformed wide-cell relationships become narrow
 * cells. The encoded cell count is the only structural field. */
template <typename ReaderT>
inline DecodeError decode(TerminalPage *page, ReaderT *reader,
                          const StyleRemap *style_remap,
                          const HyperlinkRemap *hyperlink_remap) {
    const size_t rows = page->size.rows;
    for (size_t y = 0; y < rows; y++) {
        /* Read the row header and cell count. */
        uint8_t row_header_bytes[3];
        ReadError rerr = ReadError::none;
        if (!reader->readSliceAll(row_header_bytes, 3, &rerr)) return DecodeError::EndOfStream;

        /* Every bit pattern is a valid header: booleans and the exhaustive
         * width enum decode directly, and the raw semantic value gets a
         * default below. Reserved bits do not change the known fields. */
        const Row row_header = Row::fromBits(row_header_bytes[0]);
        const uint16_t count = (uint16_t)(row_header_bytes[1] | ((uint16_t)row_header_bytes[2] << 8));

        /* A fully default row needs no work at all: decoded pages start
         * zeroed, which is exactly the default row and cell state. */
        if (row_header_bytes[0] == 0 && count == 0) continue;

        TerminalRow *row = page->getRow(y);
        TerminalRow row_value = *row;
        row_value.setWrap(row_header.wrap);
        row_value.setWrapContinuation(row_header.wrap_continuation);
        /* Wisp: std.enums.fromInt(SemanticPrompt, ...) orelse .none. */
        row_value.setSemanticPrompt(
            row_header.semantic_prompt > (uint8_t)TerminalRow::SemanticPrompt::prompt_continuation
                ? TerminalRow::SemanticPrompt::none
                : (TerminalRow::SemanticPrompt)row_header.semantic_prompt);
        *row = row_value;

        TerminalCell *cells = page->getCells(row);
        const size_t cells_len = page->size.cols;
        if (count > cells_len) return DecodeError::InvalidRowCellCount;
        if (count == 0) continue;

        /* The encoded cell width is framing: it determines exactly how many
         * bytes this row occupies. */
        DecodeError err = DecodeError::none;
        switch (row_header.cell_width) {
            case EncodedWidth::one:
            case EncodedWidth::two:
                err = decodeNarrowCells(row_header.cell_width, reader, cells, count);
                if (err != DecodeError::none) return err;
                break;

            case EncodedWidth::four:
                err = decodeWordCells(EncodedWidth::four, page, row, cells, cells_len, count,
                                      reader, style_remap, hyperlink_remap, nullptr);
                if (err != DecodeError::none) return err;
                break;

            case EncodedWidth::eight: {
                if (bulkCodec()) {
                    /* Read the encoded words directly into page storage,
                     * then normalize them in place. The raw words are only
                     * ever touched as integers until normalization makes
                     * them valid cells. */
                    if (!reader->readSliceAll((void *)cells, count * sizeof(TerminalCell), &rerr)) {
                        return DecodeError::EndOfStream;
                    }
                    uint64_t row_or = 0;
                    for (size_t x = 0; x < count; x++) {
                        const uint64_t word = cells[x].bits;
                        row_or |= word;
                        applyCell(page, row, cells, x, word, style_remap, hyperlink_remap);
                    }
                    normalizeWideRow(EncodedWidth::eight, row, cells, cells_len, count, row_or);
                } else {
                    err = decodeWordCells(EncodedWidth::eight, page, row, cells, cells_len, count,
                                          reader, style_remap, hyperlink_remap, nullptr);
                    if (err != DecodeError::none) return err;
                }

                /* The implicit cell after a short row is narrow, which
                 * resolves a trailing wide marker exactly like an explicit
                 * narrow neighbor. Only full-width rows can encode wide
                 * markers. */
                if (count < cells_len && cells[count - 1].wide() == TerminalCell::Wide::wide) {
                    cells[count - 1].setWide(TerminalCell::Wide::narrow);
                }
                break;
            }
        }
    }

    return decodeGraphemes(page, reader);
}

} /* namespace grid */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_GRID_HPP */
