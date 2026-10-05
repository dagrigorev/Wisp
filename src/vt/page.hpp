/* Transliterated from Ghostty src/terminal/page.zig and
 * src/terminal/hyperlink.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp notes:
 *   - page.zig and hyperlink.zig import each other; both are here, with the
 *     hyperlink types first and the PageEntry methods that need a complete
 *     Page defined after it. Style::bg and Style::bgCell (style.hpp) are
 *     defined at the end, once Cell is complete.
 *   - Row and Cell are packed struct(u64) upstream: here a uint64_t with an
 *     accessor pair per field at the same bit offsets (LSB first), so
 *     `row.wrap = true` is `row.setWrap(true)`.
 *   - Error sets are one enum, PageError; `none` is success.
 *   - build_options.slow_runtime_safety is WISP_SLOW_RUNTIME_SAFETY and
 *     builtin.is_test is WISP_IS_TEST (both set for the test targets).
 *     build_options.kitty_graphics is on, as in the Ghostty app.
 *   - std.heap.page_size_min is 4096 and std.atomic.cache_line is 128, their
 *     values for x86_64 Windows.
 *   - Pages are allocated with VirtualAlloc, as upstream's AllocWindows.
 */

#pragma once
#ifndef WISP_VT_PAGE_HPP
#define WISP_VT_PAGE_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>

/* Wisp: VirtualAlloc/VirtualFree declared directly rather than through
 * <windows.h>, whose RGB macro collides with the color types here. The
 * signatures match the SDK's, so a translation unit that also includes
 * <windows.h> sees compatible redeclarations. */
extern "C" __declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, unsigned long, unsigned long);
extern "C" __declspec(dllimport) int __stdcall VirtualFree(void *, size_t, unsigned long);

#include "../zigstd/allocator.hpp"
#include "bitmap_allocator.hpp"
#include "fastmem.hpp"
#include "hash_map.hpp"
#include "ref_counted_set.hpp"
#include "size.hpp"
#include "style.hpp"
#include "../zigstd/wyhash.hpp"

namespace wisp {
namespace vt {

#if defined(WISP_SLOW_RUNTIME_SAFETY) && WISP_SLOW_RUNTIME_SAFETY
static const bool slow_runtime_safety = true;
#else
static const bool slow_runtime_safety = false;
#endif

#if defined(WISP_IS_TEST) && WISP_IS_TEST
static const bool is_test = true;
#else
static const bool is_test = false;
#endif

static const size_t page_size_min = 4096;
static const size_t cache_line = 128;

/* kitty.graphics.unicode.placeholder */
static const uint32_t kitty_placeholder = 0x10EEEE;

namespace page {

/* ─── Cell / Row ─────────────────────────────────────────────────────────── */

/* A cell represents a single terminal grid cell.
 *
 * The zero value of this struct must be a valid cell representing empty,
 * since we zero initialize the backing memory for a page.
 *
 * Wisp: packed struct(u64). Bits: content_tag 0..1, content 2..25
 * (codepoint.data 2..22; color_palette.data 2..9; color_rgb r 2..9,
 * g 10..17, b 18..25), style_id 26..41, wide 42..43, protected 44,
 * hyperlink 45, semantic_content 46..47, padding 48..63. */
struct Cell {
    uint64_t bits;

    enum class ContentTag : uint8_t {
        /* A single codepoint, could be zero to be empty cell. */
        codepoint = 0,

        /* A codepoint that is part of a multi-codepoint grapheme cluster.
         * The codepoint tag is active in content, but also expect more
         * codepoints in the grapheme data. */
        codepoint_grapheme = 1,

        /* The cell has no text but only a background color. This is an
         * optimization so that cells with only backgrounds don't take up
         * style map space and also don't require a style map lookup. */
        bg_color_palette = 2,
        bg_color_rgb = 3,
    };

    struct RGB {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    enum class Wide : uint8_t {
        /* Not a wide character, cell width 1. */
        narrow = 0,

        /* Wide character, cell width 2. */
        wide = 1,

        /* Spacer after wide character. Do not render. */
        spacer_tail = 2,

        /* Spacer at the end of a soft-wrapped line to indicate that a wide
         * character is continued on the next line. */
        spacer_head = 3,
    };

    enum class SemanticContent : uint8_t {
        /* Regular output content, such as command output. */
        output = 0,

        /* Content that is part of user input, such as the command
         * to execute at a prompt. */
        input = 1,

        /* Content that is part of prompt emitted by the interactive
         * application, such as "user@host >" */
        prompt = 2,
    };

    /* Bit offsets and widths. */
    static const unsigned content_tag_off = 0, content_tag_bits = 2;
    static const unsigned content_off = 2, content_bits = 24;
    static const unsigned codepoint_bits = 21;
    static const unsigned style_id_off = 26, style_id_bits = 16;
    static const unsigned wide_off = 42, wide_bits = 2;
    static const unsigned protected_off = 44;
    static const unsigned hyperlink_off = 45;
    static const unsigned semantic_content_off = 46, semantic_content_bits = 2;

    typedef uint64_t Backing;

    Cell() : bits(0) {}

    static uint64_t fieldMaskBits(unsigned off, unsigned width) {
        return (width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1)) << off;
    }
    uint64_t get(unsigned off, unsigned width) const {
        return (bits >> off) & (((uint64_t)1 << width) - 1);
    }
    void put(unsigned off, unsigned width, uint64_t v) {
        const uint64_t m = fieldMaskBits(off, width);
        bits = (bits & ~m) | ((v << off) & m);
    }

    ContentTag content_tag() const { return (ContentTag)get(content_tag_off, content_tag_bits); }
    void setContentTag(ContentTag t) { put(content_tag_off, content_tag_bits, (uint64_t)t); }

    /* content.codepoint.data */
    uint32_t contentCodepoint() const { return (uint32_t)get(content_off, codepoint_bits); }
    /* Wisp: `.content = .{ .codepoint = .{ .data = cp } }` — sets the whole
     * content field, so the codepoint padding bits are zero. */
    void setContentCodepoint(uint32_t cp) { put(content_off, content_bits, cp & 0x1FFFFF); }

    /* content.color_palette.data */
    uint8_t contentColorPalette() const { return (uint8_t)get(content_off, 8); }
    void setContentColorPalette(uint8_t idx) { put(content_off, content_bits, idx); }

    /* content.color_rgb */
    RGB contentColorRgb() const {
        RGB c;
        c.r = (uint8_t)get(content_off, 8);
        c.g = (uint8_t)get(content_off + 8, 8);
        c.b = (uint8_t)get(content_off + 16, 8);
        return c;
    }
    void setContentColorRgb(RGB c) {
        put(content_off, content_bits,
            (uint64_t)c.r | ((uint64_t)c.g << 8) | ((uint64_t)c.b << 16));
    }

    style::Id style_id() const { return (style::Id)get(style_id_off, style_id_bits); }
    void setStyleId(style::Id id) { put(style_id_off, style_id_bits, id); }

    Wide wide() const { return (Wide)get(wide_off, wide_bits); }
    void setWide(Wide w) { put(wide_off, wide_bits, (uint64_t)w); }

    bool protected_() const { return get(protected_off, 1) != 0; }
    void setProtected(bool v) { put(protected_off, 1, v ? 1 : 0); }

    bool hyperlink() const { return get(hyperlink_off, 1) != 0; }
    void setHyperlink(bool v) { put(hyperlink_off, 1, v ? 1 : 0); }

    SemanticContent semantic_content() const {
        return (SemanticContent)get(semantic_content_off, semantic_content_bits);
    }
    void setSemanticContent(SemanticContent s) {
        put(semantic_content_off, semantic_content_bits, (uint64_t)s);
    }

    /* Returns this cell as a C ABI value. */
    uint64_t cval() const { return bits; }

    /* Helper to make a cell that just has a codepoint. */
    static Cell init(uint32_t cp) {
        /* We have to use this bitCast here to ensure that our memory is
         * zeroed. Otherwise, the content below will leave some uninitialized
         * memory in the packed union. Valgrind verifies this. */
        Cell cell;
        cell.bits = 0;
        cell.setContentTag(ContentTag::codepoint);
        cell.setContentCodepoint(cp);
        return cell;
    }

    bool isZero() const { return bits == 0; }

    /* Returns true if this cell represents a cell with text to render.
     *
     * Cases this returns false:
     *   - Cell text is blank
     *   - Cell is styled but only with a background color and no text
     *   - Cell has a unicode placeholder for Kitty graphics protocol */
    bool hasText() const {
        switch (content_tag()) {
            case ContentTag::codepoint:
            case ContentTag::codepoint_grapheme:
                return contentCodepoint() != 0;

            case ContentTag::bg_color_palette:
            case ContentTag::bg_color_rgb:
                return false;
        }
        return false;
    }

    uint32_t codepoint() const {
        switch (content_tag()) {
            case ContentTag::codepoint:
            case ContentTag::codepoint_grapheme:
                return contentCodepoint();

            case ContentTag::bg_color_palette:
            case ContentTag::bg_color_rgb:
                return 0;
        }
        return 0;
    }

    /* The width in grid cells that this cell takes up. */
    uint8_t gridWidth() const {
        switch (wide()) {
            case Wide::narrow: case Wide::spacer_head: case Wide::spacer_tail: return 1;
            case Wide::wide: return 2;
        }
        return 1;
    }

    bool hasStyling() const { return style_id() != style::default_id; }

    /* Returns true if the cell has no text or styling. */
    bool isEmpty() const {
        switch (content_tag()) {
            /* Textual cells are empty if they have no text and are narrow.
             * The "narrow" requirement is because wide spacers are meaningful. */
            case ContentTag::codepoint:
            case ContentTag::codepoint_grapheme:
                return !hasText() && wide() == Wide::narrow;

            case ContentTag::bg_color_palette:
            case ContentTag::bg_color_rgb:
                return false;
        }
        return false;
    }

    bool hasGrapheme() const { return content_tag() == ContentTag::codepoint_grapheme; }

    /* Returns true if the set of cells has text in it. */
    static bool hasTextAny(const Cell *cells, size_t len) {
        for (size_t i = 0; i < len; i++) {
            if (cells[i].hasText()) return true;
        }
        return false;
    }

    bool operator==(const Cell &o) const { return bits == o.bits; }
    bool operator!=(const Cell &o) const { return bits != o.bits; }
};
static_assert(sizeof(Cell) == 8, "Cell is 8 bytes");

/* Wisp: packed struct(u64). Bits: cells 0..31, wrap 32,
 * wrap_continuation 33, grapheme 34, styled 35, hyperlink 36,
 * semantic_prompt 37..38, kitty_virtual_placeholder 39, dirty 40,
 * padding 41..63. */
struct Row {
    uint64_t bits;

    /* The semantic prompt state of the row. See `semantic_prompt`. */
    enum class SemanticPrompt : uint8_t {
        /* No prompt cells in this row. */
        none = 0,
        /* Prompt cells exist in this row and this is a primary prompt
         * line. A primary prompt line is one that is not a continuation
         * and is the beginning of a prompt. */
        prompt = 1,
        /* Prompt cells exist in this row that had k=c set (continuation)
         * line. This is used as a way to detect when a line should
         * be considered part of some prior prompt. If no prior prompt
         * is found, the last (most historical) prompt continuation line is
         * considered the prompt. */
        prompt_continuation = 2,
    };

    static const unsigned cells_off = 0;
    static const unsigned wrap_off = 32;
    static const unsigned wrap_continuation_off = 33;
    static const unsigned grapheme_off = 34;
    static const unsigned styled_off = 35;
    static const unsigned hyperlink_off = 36;
    static const unsigned semantic_prompt_off = 37;
    static const unsigned kitty_virtual_placeholder_off = 39;
    static const unsigned dirty_off = 40;

    typedef uint64_t Backing;

    Row() : bits(0) {}

    bool flag(unsigned off) const { return (bits >> off) & 1; }
    void setFlag(unsigned off, bool v) {
        if (v) bits |= (uint64_t)1 << off; else bits &= ~((uint64_t)1 << off);
    }

    /* The cells in the row offset from the page. */
    size::Offset<Cell> cells() const { return size::Offset<Cell>((uint32_t)bits); }
    void setCells(size::Offset<Cell> o) { bits = (bits & ~(uint64_t)0xFFFFFFFFu) | o.offset; }

    /* True if this row is soft-wrapped. The first cell of the next
     * row is a continuation of this row. */
    bool wrap() const { return flag(wrap_off); }
    void setWrap(bool v) { setFlag(wrap_off, v); }

    /* True if the previous row to this one is soft-wrapped and
     * this row is a continuation of that row. */
    bool wrap_continuation() const { return flag(wrap_continuation_off); }
    void setWrapContinuation(bool v) { setFlag(wrap_continuation_off, v); }

    /* True if any of the cells in this row have multi-codepoint
     * grapheme clusters. If this is true, some fast paths are not
     * possible because erasing for example may need to clear existing
     * grapheme data. */
    bool grapheme() const { return flag(grapheme_off); }
    void setGrapheme(bool v) { setFlag(grapheme_off, v); }

    /* True if any of the cells in this row have a ref-counted style.
     * This can have false positives but never a false negative. Meaning:
     * this will be set to true the first time a style is used, but it
     * will not be set to false if the style is no longer used, because
     * checking for that condition is too expensive.
     *
     * Why have this weird false positive flag at all? This makes VT operations
     * that erase cells (such as insert lines, delete lines, erase chars,
     * etc.) MUCH MUCH faster in the case that the row was never styled.
     * At the time of writing this, the speed difference is around 4x. */
    bool styled() const { return flag(styled_off); }
    void setStyled(bool v) { setFlag(styled_off, v); }

    /* True if any of the cells in this row are part of a hyperlink.
     * This is similar to styled: it can have false positives but never
     * false negatives. This is used to optimize hyperlink operations. */
    bool hyperlink() const { return flag(hyperlink_off); }
    void setHyperlink(bool v) { setFlag(hyperlink_off, v); }

    /* The semantic prompt state for this row.
     *
     * This is ONLY meant to note if there are ANY cells in this
     * row that are part of a prompt. This is an optimization for more
     * efficiently implementing jump-to-prompt operations.
     *
     * This may contain false positives but never false negatives. If
     * this is set, you should still check individual cells to see if they
     * have prompt semantics. */
    SemanticPrompt semantic_prompt() const { return (SemanticPrompt)((bits >> semantic_prompt_off) & 3); }
    void setSemanticPrompt(SemanticPrompt s) {
        bits = (bits & ~((uint64_t)3 << semantic_prompt_off)) | ((uint64_t)s << semantic_prompt_off);
    }

    /* True if this row contains a virtual placeholder for the Kitty
     * graphics protocol. (U+10EEEE)
     * Note: We keep this as memory-using even if the kitty graphics
     * feature is disabled because we want to keep our padding and
     * everything throughout the same. */
    bool kitty_virtual_placeholder() const { return flag(kitty_virtual_placeholder_off); }
    void setKittyVirtualPlaceholder(bool v) { setFlag(kitty_virtual_placeholder_off, v); }

    /* True if this row is dirty and requires a redraw. This is set to true
     * by any operation that modifies the row's contents or position, and
     * consumers of the page are expected to clear it when they redraw.
     *
     * Dirty status is only ever meant to convey that one or more cells in
     * the row have changed visually. A cell which changes in a way that
     * doesn't affect the visual representation may not be marked as dirty.
     *
     * Dirty tracking may have false positives but should never have false
     * negatives. A false negative would result in a visual artifact on the
     * screen. */
    bool dirty() const { return flag(dirty_off); }
    void setDirty(bool v) { setFlag(dirty_off, v); }

    /* Returns this row as a C ABI value. */
    uint64_t cval() const { return bits; }

    /* Returns true if this row has any managed memory outside of the
     * row structure (graphemes, styles, etc.) */
    bool managedMemory() const {
        /* Ordered on purpose for likelihood. */
        return styled() || hyperlink() || grapheme();
    }

    /* Reset all row metadata to the default state, preserving only
     * the cells offset, and mark the row dirty. This is a single
     * 8-byte store.
     *
     * This must be applied to any row whose storage is recycled as a
     * blank row or retired into unused page capacity, in addition to
     * clearing its cells (in either order; this doesn't touch cell
     * memory). See Page.resetRow, which does both, for details. This
     * exists separately for callers that clear the cells in a
     * specialized way (e.g. filling with a background-colored blank
     * cell rather than zeroing).
     *
     * Asserts that the row has no managed memory: releasing that is
     * the cell-clearing side's job and must happen while the flags
     * are still accurate. */
    void reset() {
        /* assert(!self.managedMemory()) */
        const size::Offset<Cell> c = cells();
        bits = 0;
        setCells(c);
        setDirty(true);
    }

    static Row withCells(size::Offset<Cell> c) {
        Row r;
        r.setCells(c);
        return r;
    }
};
static_assert(sizeof(Row) == 8, "Row is 8 bytes");

/* Wisp: fieldMask for the fields of Row and Cell, by bit offset/width. */
namespace fields {
inline uint64_t mask(unsigned off, unsigned width) { return Cell::fieldMaskBits(off, width); }

/* Cell */
inline uint64_t cell_content_tag() { return mask(Cell::content_tag_off, Cell::content_tag_bits); }
inline uint64_t cell_content() { return mask(Cell::content_off, Cell::content_bits); }
inline uint64_t cell_content_codepoint_data() { return mask(Cell::content_off, Cell::codepoint_bits); }
inline uint64_t cell_style_id() { return mask(Cell::style_id_off, Cell::style_id_bits); }
inline uint64_t cell_wide() { return mask(Cell::wide_off, Cell::wide_bits); }
inline uint64_t cell_protected() { return mask(Cell::protected_off, 1); }
inline uint64_t cell_hyperlink() { return mask(Cell::hyperlink_off, 1); }
inline uint64_t cell_semantic_content() { return mask(Cell::semantic_content_off, Cell::semantic_content_bits); }

/* Row */
inline uint64_t row_cells() { return mask(Row::cells_off, 32); }
inline uint64_t row_wrap() { return mask(Row::wrap_off, 1); }
inline uint64_t row_wrap_continuation() { return mask(Row::wrap_continuation_off, 1); }
inline uint64_t row_grapheme() { return mask(Row::grapheme_off, 1); }
inline uint64_t row_styled() { return mask(Row::styled_off, 1); }
inline uint64_t row_hyperlink() { return mask(Row::hyperlink_off, 1); }
inline uint64_t row_semantic_prompt() { return mask(Row::semantic_prompt_off, 2); }
inline uint64_t row_kitty_virtual_placeholder() { return mask(Row::kitty_virtual_placeholder_off, 1); }
inline uint64_t row_dirty() { return mask(Row::dirty_off, 1); }
} /* namespace fields */

/* A comptime-generated helper for classifying and comparing packed
 * struct values (e.g. Row, Cell) in bulk, using masked compares of
 * their raw backing integers.
 *
 * Masked compares are the key to making bulk row/cell processing fast.
 * Rows and cells are small packed structs specifically so that a single
 * integer load observes every field at once. A masked compare can then
 * answer a multi-field question with one AND and one compare, instead
 * of extracting and branching on each field individually (each packed
 * field access compiles to its own shift/mask). Just as importantly,
 * the integer form vectorizes trivially: `@splat` the mask and expected
 * value, and whole groups of rows or cells can be classified with a
 * few SIMD instructions.
 *
 * T is the packed struct type, fields are the fields covered by the
 * mask, and group_len is the number of values processed at once by
 * the group (vectorized) operations. Callers typically scan a slice
 * with the group operations and fall back to the scalar variants for
 * the remainder and for pinpointing values within a matched group.
 *
 * Wisp: the mask is a run-time value built from `fields::` (upstream
 * computes it at comptime from field names); the group operations are
 * scalar loops over group_len values. */
template <typename T, size_t group_len_param>
struct Mask {
    typedef uint64_t Backing;
    uint64_t mask;

    /* The number of values processed at once by group operations. */
    static const size_t group_len = group_len_param;

    explicit Mask(uint64_t m) : mask(m) {}

    /* Returns the raw backing bits of a single value. */
    static Backing bits(const T &v) { return v.bits; }

    /* Returns the masked bits of a single value: the bits of the
     * masked fields with all other fields zeroed. Use this to
     * build the expected value for the eql functions. */
    Backing pattern(const T &v) const { return bits(v) & mask; }

    /* Returns the backing bits of a single value with the masked
     * fields zeroed: the complement of `pattern`. Use this to
     * compare values while ignoring the masked fields. */
    Backing strip(const T &v) const { return bits(v) & ~mask; }

    /* Returns true if every value in the group of group_len
     * values starting at index i matches, where a value matches
     * when none of the masked fields have any bits set: false
     * for bools, zero for ints, the zero tag for enums, and so
     * on. Asserts that at least group_len values are available. */
    bool match(const T *values, size_t i) const {
        uint64_t acc = 0;
        for (size_t k = 0; k < group_len; k++) acc |= values[i + k].bits;
        return (acc & mask) == 0;
    }

    /* Scalar variant of `match` for a single value. */
    bool matchScalar(const T &v) const { return (bits(v) & mask) == 0; }

    /* Returns true if the masked fields of every value in the
     * group of group_len values starting at index i equal the
     * expected pattern (see `pattern`).
     *
     * This is a masked compare: fields outside the mask may vary
     * freely. Use this to detect runs of values that share the
     * masked field contents while other fields differ, e.g. a run
     * of cells with the same style ID but different codepoints.
     * If the result you derive from a run depends on fields
     * outside the mask, use `eqlExact` instead. */
    bool eql(const T *values, size_t i, Backing expected) const {
        for (size_t k = 0; k < group_len; k++) {
            if ((values[i + k].bits & mask) != expected) return false;
        }
        return true;
    }

    /* Scalar variant of `eql` for a single value. */
    bool eqlScalar(const T &v, Backing expected) const { return pattern(v) == expected; }

    /* Returns true if any value in the group of group_len values
     * starting at index i has masked fields equal to the expected
     * pattern (see `pattern`). This is the "any" counterpart to
     * `eql`: use it to detect the presence of a specific value
     * within a group, e.g. a run scan that must stop when it
     * encounters a sentinel codepoint anywhere in the group. */
    bool eqlAny(const T *values, size_t i, Backing expected) const {
        for (size_t k = 0; k < group_len; k++) {
            if ((values[i + k].bits & mask) == expected) return true;
        }
        return false;
    }

    /* Like `eql` but returns the number of leading values whose
     * masked fields equal the expected pattern, i.e. group_len if
     * the entire group matches. This is useful for early-exit run
     * scans that need to pinpoint exactly where a run ends rather
     * than only whether the whole group matches. */
    size_t eqlPrefix(const T *values, size_t i, Backing expected) const {
        for (size_t k = 0; k < group_len; k++) {
            if ((values[i + k].bits & mask) != expected) return k;
        }
        return group_len;
    }

    /* Returns true if every value in the group of group_len
     * values starting at index i is bit-identical to the expected
     * value. Note: this compares entire values; it is NOT
     * affected by the field mask. */
    bool eqlExact(const T *values, size_t i, Backing expected) const {
        for (size_t k = 0; k < group_len; k++) {
            if (values[i + k].bits != expected) return false;
        }
        return true;
    }
};

struct Page;

} /* namespace page */

/* ─── hyperlink.zig ──────────────────────────────────────────────────────── */

namespace hyperlink {

/* The unique identifier for a hyperlink. This is at most the number of cells
 * that can fit in a single terminal page, since each cell can only contain
 * at most one hyperlink. */
typedef size::HyperlinkCountInt Id;

/* The mapping of cell to hyperlink. We use an offset hash map to save space
 * since its very unlikely a cell is a hyperlink, so its a waste to store
 * the hyperlink ID in the cell itself. */
typedef hash_map::AutoOffsetHashMap<size::Offset<page::Cell>, Id, 80>::Type Map;

/* A fully decoded hyperlink that may or may not have its
 * memory within a page. The memory location of this is dependent
 * on the context so users should check with the source of the
 * hyperlink.
 *
 * Wisp: slices are pointer plus length. */
struct Hyperlink {
    /* See PageEntry.Id */
    struct Id {
        enum class Tag : uint8_t { explicit_, implicit };
        Tag tag;
        const uint8_t *explicit_ptr;
        size_t explicit_len;
        size::OffsetInt implicit;

        static Id makeExplicit(const uint8_t *p, size_t n) {
            Id id;
            id.tag = Tag::explicit_;
            id.explicit_ptr = p;
            id.explicit_len = n;
            id.implicit = 0;
            return id;
        }
        static Id makeImplicit(size::OffsetInt v) {
            Id id;
            id.tag = Tag::implicit;
            id.explicit_ptr = nullptr;
            id.explicit_len = 0;
            id.implicit = v;
            return id;
        }
    };

    Id id;
    const uint8_t *uri;
    size_t uri_len;

    /* Deinit and deallocate all the pointers using the given
     * allocator.
     *
     * WARNING: This should only be called if the hyperlink was
     * heap-allocated. This DOES NOT need to be unconditionally
     * called. */
    void deinit(zigstd::Allocator alloc) const {
        alloc.free((uint8_t *)uri, uri_len);
        switch (id.tag) {
        case Id::Tag::implicit: break;
        case Id::Tag::explicit_: alloc.free((uint8_t *)id.explicit_ptr, id.explicit_len); break;
        }
    }

    /* Duplicate a hyperlink by allocating all values with the
     * given allocator. The returned hyperlink should have deinit
     * called. Wisp: false is OutOfMemory. */
    bool dupe(zigstd::Allocator alloc, Hyperlink *out) const {
        uint8_t *u = alloc.dupe(uri, uri_len);
        if (!u) return false;

        Id nid = id;
        switch (id.tag) {
        case Id::Tag::implicit: break;
        case Id::Tag::explicit_: {
            uint8_t *e = alloc.dupe(id.explicit_ptr, id.explicit_len);
            if (!e) {
                alloc.free(u, uri_len);
                return false;
            }
            nid.explicit_ptr = e;
            break;
        }
        }

        out->id = nid;
        out->uri = u;
        out->uri_len = uri_len;
        return true;
    }
};

typedef size::Offset<uint8_t>::Slice StrSlice;

/* A hyperlink that has been committed to page memory. This
 * is a "page entry" because while it represents a hyperlink,
 * some decoding (pointer chasing) is still necessary to get the
 * fully realized ID, URI, etc. */
struct PageEntry {
    struct Id {
        enum class Tag : uint8_t {
            /* An explicitly provided ID via the OSC8 sequence. */
            explicit_,

            /* No ID was provided so we auto-generate the ID based on an
             * incrementing counter attached to the screen. */
            implicit,
        };

        /* Wisp: union(enum) — payload then tag, 24 bytes as upstream. */
        union {
            StrSlice explicit_;
            size::OffsetInt implicit;
        };
        Tag tag;

        Id() : explicit_(), tag(Tag::explicit_) {}

        static Id makeExplicit(StrSlice s) {
            Id id;
            id.tag = Tag::explicit_;
            id.explicit_ = s;
            return id;
        }
        static Id makeImplicit(size::OffsetInt v) {
            Id id;
            id.tag = Tag::implicit;
            id.implicit = v;
            return id;
        }
    };

    Id id;
    StrSlice uri;

    PageEntry() : id(), uri() {}

    /* Duplicate this hyperlink from one page to another.
     * Wisp: false is OutOfMemory. Defined after Page. */
    bool dupe(const page::Page *self_page, page::Page *dst_page, PageEntry *out) const;

    template <typename B>
    uint64_t hash(const B &base) const {
        zigstd::Wyhash hasher = zigstd::Wyhash::init(0);
        /* autoHash(&hasher, std.meta.activeTag(self.id)): the tag is an
         * enum(u1), hashed as its one byte. */
        const uint8_t tag = (uint8_t)id.tag;
        hasher.update(&tag, 1);
        switch (id.tag) {
            case Id::Tag::implicit: {
                /* autoHash(&hasher, v): u32, little-endian bytes. */
                const uint32_t v = id.implicit;
                uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
                hasher.update(b, 4);
                break;
            }
            case Id::Tag::explicit_:
                hashSliceDeep(&hasher, id.explicit_.slice(base), id.explicit_.len);
                break;
        }
        hashSliceDeep(&hasher, uri.slice(base), uri.len);
        return hasher.final_();
    }

    /* Wisp: autoHashStrat(hasher, []const u8, .Deep) — each byte, then the
     * length as a usize. */
    static void hashSliceDeep(zigstd::Wyhash *hasher, const uint8_t *p, size_t len) {
        if (len) hasher->update(p, len);
        const uint64_t l = len;
        uint8_t b[8];
        for (int k = 0; k < 8; k++) b[k] = (uint8_t)(l >> (8 * k));
        hasher->update(b, 8);
    }

    template <typename B1, typename B2>
    bool eql(const B1 &self_base, const PageEntry &other, const B2 &other_base) const {
        if (id.tag != other.id.tag) return false;
        switch (id.tag) {
            case Id::Tag::implicit:
                if (id.implicit != other.id.implicit) return false;
                break;
            case Id::Tag::explicit_: {
                const uint8_t *self_ptr = id.explicit_.offset.ptr(self_base);
                const uint8_t *other_ptr = other.id.explicit_.offset.ptr(other_base);
                if (id.explicit_.len != other.id.explicit_.len) return false;
                if (id.explicit_.len && memcmp(self_ptr, other_ptr, id.explicit_.len) != 0) return false;
                break;
            }
        }

        if (uri.len != other.uri.len) return false;
        return uri.len == 0 || memcmp(uri.slice(self_base), other.uri.slice(other_base), uri.len) == 0;
    }

    /* Free the memory for this entry from its page. Defined after Page. */
    void free_(page::Page *pg) const;
};

/* The set of hyperlinks. This is ref-counted so that a set of cells
 * can share the same hyperlink without duplicating the data. */
struct SetContext {
    /* The page which holds the strings for items in this set. */
    page::Page *page; /* = null */

    /* The page which holds the strings for items
     * looked up with, e.g., `add` or `lookup`,
     * if different from the destination page. */
    const page::Page *src_page; /* = null */

    SetContext() : page(nullptr), src_page(nullptr) {}
    explicit SetContext(page::Page *p, const page::Page *src = nullptr) : page(p), src_page(src) {}

    uint64_t hash(const PageEntry &link) const;
    bool eql(const PageEntry &a, const PageEntry &b) const;
    void deleted(const PageEntry &link) const;
};

typedef ref_counted_set::RefCountedSet<PageEntry, Id, size::CellCountInt, SetContext> Set;

} /* namespace hyperlink */

/* ─── page.zig ───────────────────────────────────────────────────────────── */

namespace page {

/* The allocator to use for multi-codepoint grapheme data. We use
 * a chunk size of 4 codepoints. It'd be best to set this empirically
 * but it is currently set based on vibes. My thinking around 4 codepoints
 * is that most skin-tone emoji are <= 4 codepoints, letter combiners
 * are usually <= 4 codepoints, and 4 codepoints is a nice power of two
 * for alignment. */
static const size_t grapheme_chunk_len = 4;
static const size_t grapheme_chunk = grapheme_chunk_len * sizeof(uint32_t);
static const size_t grapheme_max_len = 64;
typedef bitmap_allocator::BitmapAllocator<grapheme_chunk> GraphemeAlloc;
static const size_t grapheme_count_default = GraphemeAlloc::bitmap_bit_size;
static const size_t grapheme_bytes_default = grapheme_count_default * grapheme_chunk;
typedef hash_map::AutoOffsetHashMap<size::Offset<Cell>, size::Offset<uint32_t>::Slice,
                                    hash_map::default_max_load_percentage>::Type GraphemeMap;

/* The allocator used for shared utf8-encoded strings within a page.
 * Note the chunk size below is the minimum size of a single allocation
 * and requires a single bit of metadata in our bitmap allocator. Therefore
 * it should be tuned carefully (too small and we waste metadata, too large
 * and we have fragmentation). We can probably use a better allocation
 * strategy in the future.
 *
 * At the time of writing this, the strings table is only used for OSC8
 * IDs and URIs. IDs are usually short and URIs are usually longer. I chose
 * 32 bytes as a compromise between these two since it represents single
 * domain links quite well and is not too wasteful for short IDs. We can
 * continue to tune this as we see how it's used. */
static const size_t string_chunk_len = 32;
static const size_t string_chunk = string_chunk_len * sizeof(uint8_t);
typedef bitmap_allocator::BitmapAllocator<string_chunk> StringAlloc;
static const size_t string_count_default = StringAlloc::bitmap_bit_size;
static const size_t string_bytes_default = string_count_default * string_chunk;

/* Default number of hyperlinks we support.
 *
 * The cell multiplier is the number of cells per hyperlink entry that
 * we support. A hyperlink can be longer than this multiplier; the multiplier
 * just sets the total capacity to simplify adjustable size metrics. */
static const size_t hyperlink_count_default = 4;
static const size_t hyperlink_bytes_default = hyperlink_count_default * sizeof(hyperlink::Set::Item);
static const size_t hyperlink_cell_multiplier = 16;

/* The alignment of the start of a page's cell array. Align it to a cache
 * line so that row cells always start on a cache line. This avoids
 * a scenario where cells in every row always start mid-cache line and
 * straddle an extra. */
static const size_t cells_align =
    alignof(Cell) > (cache_line < page_size_min ? cache_line : page_size_min)
        ? alignof(Cell)
        : (cache_line < page_size_min ? cache_line : page_size_min);

typedef style::Set StyleSet;

inline size_t alignForward(size_t v, size_t a) { return (v + a - 1) / a * a; }

/* The size of this page. */
struct Size {
    size::CellCountInt cols;
    size::CellCountInt rows;
};

/* Capacity of this page.
 *
 * This capacity can be maxed out (every field max) and still fit
 * within a 64-bit memory space. If you need more than this, you will
 * need to split data across separate pages.
 *
 * For 32-bit systems, it is possible to overflow the addressable
 * space and this is something we still need to address in the future
 * likely by limiting the maximum capacity on 32-bit systems further. */
struct Capacity {
    /* Number of columns and rows we can know about. */
    size::CellCountInt cols;
    size::CellCountInt rows;

    /* Number of unique styles that can be used on this page. */
    size::StyleCountInt styles; /* = 16 */

    /* Number of bytes to allocate for hyperlink data. Note that the
     * amount of data used for hyperlinks in total is more than this because
     * hyperlinks use string data as well as a small amount of lookup metadata.
     * This number is a rough approximation. */
    size::HyperlinkCountInt hyperlink_bytes; /* = hyperlink_bytes_default */

    /* Number of bytes to allocate for grapheme data. */
    size::GraphemeBytesInt grapheme_bytes; /* = grapheme_bytes_default */

    /* Number of bytes to allocate for strings. */
    size::StringBytesInt string_bytes; /* = string_bytes_default */

    Capacity()
        : cols(0), rows(0), styles(16), hyperlink_bytes((size::HyperlinkCountInt)hyperlink_bytes_default),
          grapheme_bytes((size::GraphemeBytesInt)grapheme_bytes_default),
          string_bytes((size::StringBytesInt)string_bytes_default) {}

    Capacity(size::CellCountInt c, size::CellCountInt r) : Capacity() {
        cols = c;
        rows = r;
    }

    bool operator==(const Capacity &o) const {
        return cols == o.cols && rows == o.rows && styles == o.styles &&
               hyperlink_bytes == o.hyperlink_bytes && grapheme_bytes == o.grapheme_bytes &&
               string_bytes == o.string_bytes;
    }
    bool operator!=(const Capacity &o) const { return !(*this == o); }

    struct Adjustment {
        bool has_cols; /* cols: ?size.CellCountInt = null */
        size::CellCountInt cols;
        Adjustment() : has_cols(false), cols(0) {}
        static Adjustment withCols(size::CellCountInt c) {
            Adjustment a;
            a.has_cols = true;
            a.cols = c;
            return a;
        }
    };

    /* Returns the maximum number of columns that can be used with this
     * capacity while still fitting at least one row. Returns null if even
     * a single column cannot fit (which would indicate an unusable capacity).
     *
     * Note that this is the maximum number of columns that never increases
     * the amount of memory the original capacity will take. If you modify
     * the original capacity to add rows, then you can fit more columns. */
    bool maxCols(size::CellCountInt *out) const;

    /* Adjust the capacity parameters while retaining the same total size.
     *
     * Adjustments always happen by limiting the rows in the page. Everything
     * else can grow. If it is impossible to achieve the desired adjustment,
     * OutOfMemory is returned. Wisp: false is OutOfMemory. */
    bool adjust(Adjustment req, Capacity *out) const;

    /* Computes the number of bytes available for the row headers and
     * cells in the page: the page size minus the metadata block. */
    size_t availableBytesForGrid() const;
};

/* The standard capacity for a page that doesn't have special
 * requirements. This is enough to support a very large number of cells.
 * The standard capacity is chosen as the fast-path for allocation since
 * pages of standard capacity use a pooled allocator instead of single-use
 * mmaps. */
inline Capacity std_capacity() {
    Capacity c;
    c.cols = 215;
    c.rows = 215;
    c.styles = 128;
    c.grapheme_bytes = is_test ? 512 : 8192;
    return c;
}

/* Wisp: the error sets of this file, flattened. */
enum class PageError : uint8_t {
    none,

    /* Allocator.Error */
    OutOfMemory,

    /* IntegrityError */
    ZeroRowCount,
    ZeroColCount,
    UnmarkedGraphemeRow,
    MissingGraphemeData,
    InvalidGraphemeCount,
    UnmarkedGraphemeCell,
    MissingStyle,
    UnmarkedStyleRow,
    MismatchedStyleRef,
    InvalidStyleCount,
    MissingHyperlinkData,
    MismatchedHyperlinkRef,
    UnmarkedHyperlinkCell,
    UnmarkedHyperlinkRow,
    InvalidSpacerTailLocation,
    InvalidSpacerHeadLocation,
    UnwrappedSpacerHead,

    /* StyleSetError */
    StyleSetOutOfMemory,
    StyleSetNeedsRehash,

    /* HyperlinkError */
    StringAllocOutOfMemory,
    HyperlinkSetOutOfMemory,
    HyperlinkSetNeedsRehash,
    HyperlinkMapOutOfMemory,

    /* GraphemeError */
    GraphemeMapOutOfMemory,
    GraphemeAllocOutOfMemory,

    /* InsertHyperlinkError */
    StringsOutOfMemory,
    SetOutOfMemory,
    SetNeedsRehash,
};

/* Page-aligned allocator used for terminal page backing memory. Pages
 * require page-aligned, zeroed memory obtained directly from the OS
 * (not the Zig allocator) because the allocation fast-path is
 * performance-critical and the OS guarantees zeroed pages.
 *
 * Allocate page-aligned, zeroed backing memory using VirtualAlloc with
 * MEM_COMMIT | MEM_RESERVE which guarantees zeroed pages. */
struct PageAlloc {
    static uint8_t *alloc(size_t n) {
        /* MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE */
        return (uint8_t *)::VirtualAlloc(nullptr, n, 0x00001000 | 0x00002000, 0x04);
    }

    /* MEM_RELEASE */
    static void free(uint8_t *mem) { ::VirtualFree(mem, 0, 0x00008000); }
};

/* A page represents a specific section of terminal screen. The primary
 * idea of a page is that it is a fully self-contained unit that can be
 * serialized, copied, etc. as a convenient way to represent a section
 * of the screen.
 *
 * This property is useful for renderers which want to copy just the pages
 * for the visible portion of the screen, or for infinite scrollback where
 * we may want to serialize and store pages that are sufficiently far
 * away from the current viewport.
 *
 * Pages are always backed by a single contiguous block of memory that is
 * aligned on a page boundary. This makes it easy and fast to copy pages
 * around. Within the contiguous block of memory, the contents of a page are
 * thoughtfully laid out to optimize primarily for terminal IO (VT streams)
 * and to minimize memory usage. */
struct Page {
    /* The backing memory for the page. A page is always made up of a
     * a single contiguous block of memory that is aligned on a page
     * boundary and is a multiple of the system page size.
     * Wisp: []align(page_size_min) u8 is memory plus memory_len. */
    uint8_t *memory;
    size_t memory_len;

    /* The array of rows in the page. The rows are always in row order
     * (i.e. index 0 is the top row, index 1 is the row below that, etc.) */
    size::Offset<Row> rows;

    /* The array of cells in the page. The cells are NOT in row order,
     * but they are in column order. To determine the mapping of cells
     * to row, you must use the `rows` field. From the pointer to the
     * first column, all cells in that row are laid out in column order. */
    size::Offset<Cell> cells;

    /* Set to true when an operation is performed that dirties all rows in
     * the page. See `Row.dirty` for more information on dirty tracking.
     *
     * NOTE: A value of false does NOT indicate that
     *       the page has no dirty rows in it, only
     *       that no full-page-dirtying operations
     *       have occurred since it was last cleared. */
    bool dirty;

    /* The string allocator for this page used for shared utf-8 encoded
     * strings. Liveness of strings and memory management is deferred to
     * the individual use case. */
    StringAlloc string_alloc;

    /* The multi-codepoint grapheme data for this page. This is where
     * any cell that has more than one codepoint will be stored. This is
     * relatively rare (typically only emoji) so this defaults to a very small
     * size and we force page realloc when it grows. */
    GraphemeAlloc grapheme_alloc;

    /* The mapping of cell to grapheme data. The exact mapping is the
     * cell offset to the grapheme data offset. Therefore, whenever a
     * cell is moved (i.e. `erase`) then the grapheme data must be updated.
     * Grapheme data is relatively rare so this is considered a slow
     * path. */
    GraphemeMap grapheme_map;

    /* The available set of styles in use on this page. */
    StyleSet styles;

    /* The structures used for tracking hyperlinks within the page.
     * The map maps cell offsets to hyperlink IDs and the IDs are in
     * the ref counted set. The strings within the hyperlink structures
     * are allocated in the string allocator. */
    hyperlink::Map hyperlink_map;
    hyperlink::Set hyperlink_set;

    /* The current dimensions of the page. The capacity may be larger
     * than this. This allows us to allocate a larger page than necessary
     * and also to resize a page smaller without reallocating. */
    Size size;

    /* The capacity of this page. This is the full size of the backing
     * memory and is fixed at page creation time. */
    Capacity capacity;

    /* If this is true then verifyIntegrity will do nothing. This is
     * only present with runtime safety enabled. */
    size_t pause_integrity_checks;

    struct Layout {
        size_t total_size;
        size_t rows_start;
        size_t rows_size;
        size_t cells_start;
        size_t cells_size;
        size_t styles_start;
        StyleSet::Layout styles_layout;
        size_t grapheme_alloc_start;
        GraphemeAlloc::Layout grapheme_alloc_layout;
        size_t grapheme_map_start;
        GraphemeMap::Layout grapheme_map_layout;
        size_t string_alloc_start;
        StringAlloc::Layout string_alloc_layout;
        size_t hyperlink_map_start;
        hyperlink::Map::Layout hyperlink_map_layout;
        size_t hyperlink_set_start;
        hyperlink::Set::Layout hyperlink_set_layout;
        Capacity capacity;
    };

    /* Meta is everything that isn't the grid, such as styles, graphemes,
     * etc. `layout` places it directly after the grid. */
    struct MetaLayout {
        /* The size of the block including internal alignment padding. */
        size_t total_size;
        size_t styles_start;
        StyleSet::Layout styles_layout;
        size_t grapheme_alloc_start;
        GraphemeAlloc::Layout grapheme_alloc_layout;
        size_t grapheme_map_start;
        GraphemeMap::Layout grapheme_map_layout;
        size_t string_alloc_start;
        StringAlloc::Layout string_alloc_layout;
        size_t hyperlink_set_start;
        hyperlink::Set::Layout hyperlink_set_layout;
        size_t hyperlink_map_start;
        hyperlink::Map::Layout hyperlink_map_layout;

        /* The alignment of the block's start: the largest alignment any
         * member requires, so that the member offsets above don't depend
         * on where the block is placed. */
        static size_t alignment() {
            size_t a = StyleSet::base_align;
            if (GraphemeAlloc::base_align > a) a = GraphemeAlloc::base_align;
            if (GraphemeMap::base_align > a) a = GraphemeMap::base_align;
            if (StringAlloc::base_align > a) a = StringAlloc::base_align;
            if (hyperlink::Set::base_align > a) a = hyperlink::Set::base_align;
            if (hyperlink::Map::base_align > a) a = hyperlink::Map::base_align;
            return a;
        }

        /* Compute the layout of the block for the given capacity. */
        static MetaLayout init(const Capacity &cap) {
            MetaLayout m;
            m.styles_layout = StyleSet::Layout::init(cap.styles);
            m.styles_start = 0;
            const size_t styles_end = m.styles_start + m.styles_layout.total_size;

            m.grapheme_alloc_layout = GraphemeAlloc::layout(cap.grapheme_bytes);
            m.grapheme_alloc_start = alignForward(styles_end, GraphemeAlloc::base_align);
            const size_t grapheme_alloc_end = m.grapheme_alloc_start + m.grapheme_alloc_layout.total_size;

            size_t grapheme_count = 0;
            if (cap.grapheme_bytes != 0) {
                /* Use divCeil to match GraphemeAlloc.layout() which uses alignForward,
                 * ensuring grapheme_map has capacity when grapheme_alloc has chunks. */
                const size_t base = (cap.grapheme_bytes + grapheme_chunk - 1) / grapheme_chunk;
                size_t p = 1;
                while (p < base) p <<= 1;
                grapheme_count = p;
            }
            m.grapheme_map_layout = GraphemeMap::layout((uint32_t)grapheme_count);
            m.grapheme_map_start = alignForward(grapheme_alloc_end, GraphemeMap::base_align);
            const size_t grapheme_map_end = m.grapheme_map_start + m.grapheme_map_layout.total_size;

            m.string_alloc_layout = StringAlloc::layout(cap.string_bytes);
            m.string_alloc_start = alignForward(grapheme_map_end, StringAlloc::base_align);
            const size_t string_end = m.string_alloc_start + m.string_alloc_layout.total_size;

            const size_t hyperlink_count = cap.hyperlink_bytes / sizeof(hyperlink::Set::Item);
            m.hyperlink_set_layout = hyperlink::Set::Layout::init(hyperlink_count);
            m.hyperlink_set_start = alignForward(string_end, hyperlink::Set::base_align);
            const size_t hyperlink_set_end = m.hyperlink_set_start + m.hyperlink_set_layout.total_size;

            uint32_t hyperlink_map_count = 0;
            if (hyperlink_count != 0) {
                const uint64_t mult = (uint64_t)hyperlink_count * hyperlink_cell_multiplier;
                hyperlink_map_count = mult > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)mult;
            }
            m.hyperlink_map_layout = hyperlink::Map::layout(hyperlink_map_count);
            m.hyperlink_map_start = alignForward(hyperlink_set_end, hyperlink::Map::base_align);
            const size_t hyperlink_map_end = m.hyperlink_map_start + m.hyperlink_map_layout.total_size;

            m.total_size = hyperlink_map_end;
            return m;
        }
    };

    /* The memory layout for a page given a desired minimum cols
     * and rows size.
     *
     * The backing memory is laid out as the row headers, the cell
     * array, and then the metadata block (see `MetaLayout`):
     *
     *   [rows][cells][styles, graphemes, strings, hyperlinks]
     *
     * Row headers always start at offset zero. The cell array is aligned
     * to a cache line (see `cells_align`). Initializing a page writes only
     * the row headers: the cells and every metadata member treat zero as
     * their empty state, so the OS pages behind everything past the row
     * headers stay untouched until something is stored in them. */
    static Layout layout(const Capacity &cap) {
        const MetaLayout meta = MetaLayout::init(cap);

        const size_t rows_count = cap.rows;
        const size_t rows_start = 0;
        const size_t rows_end = rows_start + (rows_count * sizeof(Row));

        const size_t cells_count = (size_t)cap.cols * (size_t)cap.rows;
        const size_t cells_start = alignForward(rows_end, cells_align);
        const size_t cells_end = cells_start + (cells_count * sizeof(Cell));

        const size_t meta_start = alignForward(cells_end, MetaLayout::alignment());
        const size_t meta_end = meta_start + meta.total_size;

        const size_t total_size = alignForward(meta_end, page_size_min);

        Layout l;
        l.total_size = total_size;
        l.rows_start = rows_start;
        l.rows_size = rows_end - rows_start;
        l.cells_start = cells_start;
        l.cells_size = cells_end - cells_start;
        l.styles_start = meta_start + meta.styles_start;
        l.styles_layout = meta.styles_layout;
        l.grapheme_alloc_start = meta_start + meta.grapheme_alloc_start;
        l.grapheme_alloc_layout = meta.grapheme_alloc_layout;
        l.grapheme_map_start = meta_start + meta.grapheme_map_start;
        l.grapheme_map_layout = meta.grapheme_map_layout;
        l.string_alloc_start = meta_start + meta.string_alloc_start;
        l.string_alloc_layout = meta.string_alloc_layout;
        l.hyperlink_map_start = meta_start + meta.hyperlink_map_start;
        l.hyperlink_map_layout = meta.hyperlink_map_layout;
        l.hyperlink_set_start = meta_start + meta.hyperlink_set_start;
        l.hyperlink_set_layout = meta.hyperlink_set_layout;
        l.capacity = cap;
        return l;
    }

    /* Initialize a new page, allocating the required backing memory.
     * The size of the initialized page defaults to the full capacity.
     *
     * The backing memory is always allocated using mmap directly.
     * You cannot use custom allocators with this structure because
     * it is critical to performance that we use mmap.
     *
     * Wisp: false is OutOfMemory. */
    static bool init(const Capacity &cap, Page *out) {
        const Layout l = layout(cap);

        /* We allocate page-aligned zeroed memory directly to avoid Zig
         * allocator overhead (small but meaningful for this path). Both
         * mmap (POSIX) and VirtualAlloc (Windows) guarantee zeroed pages,
         * which is a critical property for us.
         * assert(l.total_size % std.heap.page_size_min == 0) */
        uint8_t *backing = PageAlloc::alloc(l.total_size);
        if (!backing) return false;

        const size::OffsetBuf buf = size::OffsetBuf::init(backing);
        *out = initBuf(buf, l);
        return true;
    }

    /* Initialize a new page using the given backing memory.
     * It is up to the caller to not call deinit on these pages.
     *
     * The backing memory must be zero-filled. A page treats zero as the
     * empty state everywhere: cells are blank when zero, and every
     * metadata member initializes from zeroed memory without writing
     * anything. Only the row headers are written, so the OS pages behind
     * everything else stay untouched until first use. */
    static Page initBuf(size::OffsetBuf buf, const Layout &l) {
        const Capacity cap = l.capacity;

        /* A page must always have at least one row.
         * assert(cap.rows > 0) */

        const size::Offset<Row> r = buf.member<Row>(l.rows_start);
        const size::Offset<Cell> c = buf.member<Cell>(l.cells_start);

        /* We need to go through and initialize all the rows so that
         * they point to a valid offset into the cells, since the rows
         * zero-initialized aren't valid. */
        Cell *cells_ptr = c.ptr(buf);
        Row *rows_ptr = r.ptr(buf);
        for (size_t y = 0; y < cap.rows; y++) {
            const size_t start = y * cap.cols;
            rows_ptr[y] = Row::withCells(size::getOffset<Cell>(buf, &cells_ptr[start]));
        }

        Page p;
        p.memory = buf.start();
        p.memory_len = l.total_size;
        p.rows = r;
        p.cells = c;
        p.styles = StyleSet::initAssumeZeroed(buf.add(l.styles_start), l.styles_layout, style::SetContext());
        p.string_alloc = StringAlloc::initAssumeZeroed(buf.add(l.string_alloc_start), l.string_alloc_layout);
        p.grapheme_alloc = GraphemeAlloc::initAssumeZeroed(buf.add(l.grapheme_alloc_start), l.grapheme_alloc_layout);
        p.grapheme_map = GraphemeMap::initAssumeZeroed(buf.add(l.grapheme_map_start), l.grapheme_map_layout);
        p.hyperlink_map = hyperlink::Map::initAssumeZeroed(buf.add(l.hyperlink_map_start), l.hyperlink_map_layout);
        p.hyperlink_set = hyperlink::Set::initAssumeZeroed(buf.add(l.hyperlink_set_start), l.hyperlink_set_layout,
                                                          hyperlink::SetContext());
        p.size.cols = cap.cols;
        p.size.rows = cap.rows;
        p.capacity = cap;
        p.dirty = false;
        p.pause_integrity_checks = 0;
        return p;
    }

    /* Deinitialize the page, freeing any backing memory. Do NOT call
     * this if you allocated the backing memory yourself (i.e. you used
     * initBuf). */
    void deinit() {
        PageAlloc::free(memory);
        memory = nullptr;
        memory_len = 0;
    }

    /* Reinitialize the page with the same capacity. */
    void reinit() {
        /* We zero the page memory as u64 instead of u8 because
         * we can and it's empirically quite a bit faster. */
        memset(memory, 0, memory_len / 8 * 8);
        *this = initBuf(size::OffsetBuf::init(memory), layout(capacity));
    }

    /* Temporarily pause integrity checks. This is useful when you are
     * doing a lot of operations that would trigger integrity check
     * violations but you know the page will end up in a consistent state. */
    void pauseIntegrityChecks(bool v) {
        if (slow_runtime_safety) {
            if (v) {
                pause_integrity_checks += 1;
            } else {
                pause_integrity_checks -= 1;
            }
        }
    }

    /* A helper that can be used to assert the integrity of the page when
     * runtime safety is enabled. This is a no-op when runtime safety is
     * disabled or the target is freestanding. This uses the libc allocator. */
    void assertIntegrity() const {
        if (slow_runtime_safety) {
            const PageError err = verifyIntegrity();
            if (err != PageError::none) {
                fprintf(stderr, "page integrity violation, crashing. err=%d\n", (int)err);
                abort();
            }
        }
    }

    /* Verifies the integrity of the page data. This is not fast,
     * but it is useful for assertions, deserialization, etc. The
     * allocator is only used for temporary allocations -- all memory
     * is freed before this function returns.
     *
     * Integrity errors are also logged as warnings. */
    PageError verifyIntegrity() const;

    /* Clone the contents of this page. This will allocate new memory
     * using the page allocator. If you want to manage memory manually,
     * use cloneBuf. Wisp: false is OutOfMemory. */
    bool clone(Page *out) const {
        uint8_t *backing = PageAlloc::alloc(memory_len);
        if (!backing) return false;
        *out = cloneBuf(backing, memory_len);
        return true;
    }

    /* Clone the entire contents of this page.
     *
     * The buffer must be at least the size of self.memory. */
    Page cloneBuf(uint8_t *buf, size_t buf_len) const {
        /* assert(buf.len >= self.memory.len) */
        (void)buf_len;

        /* The entire concept behind a page is that everything is stored
         * as offsets so we can do a simple linear copy of the backing
         * memory and copy all the offsets and everything will work. */
        Page result = *this;
        result.memory = buf;
        result.memory_len = memory_len;

        /* This is a memcpy. We may want to investigate if there are
         * faster ways to do this (i.e. copy-on-write tricks) but I suspect
         * they'll be slower. I haven't experimented though. */
        fastmem::copy<uint8_t>(result.memory, memory, memory_len);

        return result;
    }

    /* Compute the exact capacity required to store a range of rows from
     * this page.
     *
     * The returned capacity will have the same number of columns as this
     * page and the number of rows equal to the range given. The returned
     * capacity is by definition strictly less than or equal to this
     * page's capacity, so the layout is guaranteed to succeed.
     *
     * Preconditions:
     * - Range must be at least 1 row
     * - Start and end must be valid for this page */
    Capacity exactRowCapacity(size_t y_start, size_t y_end) const;

    /* Clone the contents of another page into this page. The capacities
     * can be different, but the size of the other page must fit into
     * this page.
     *
     * The y_start and y_end parameters allow you to clone only a portion
     * of the other page. This is useful for splitting a page into two
     * or more pages.
     *
     * The column count of this page will always be the same as this page.
     * If the other page has more columns, the extra columns will be
     * truncated. If the other page has fewer columns, the extra columns
     * will be zeroed. */
    PageError cloneFrom(const Page *other, size_t y_start, size_t y_end) {
        /* assert(y_start <= y_end); assert(y_end <= other.size.rows);
         * assert(y_end - y_start <= self.size.rows) */

        Row *other_rows = other->rows.ptr(other->memory) + y_start;
        Row *rws = rows.ptr(memory);
        for (size_t i = 0; i < y_end - y_start; i++) {
            const PageError e = cloneRowFrom(other, &rws[i], &other_rows[i]);
            if (e != PageError::none) return e;
        }

        /* We should remain consistent */
        assertIntegrity();
        return PageError::none;
    }

    /* Clone a single row from another page into this page. */
    PageError cloneRowFrom(const Page *other, Row *dst_row, const Row *src_row) {
        return clonePartialRowFrom(other, dst_row, src_row, 0, size.cols);
    }

    /* Clone a single row from another page into this page, supporting
     * partial copy. cloneRowFrom calls this. */
    PageError clonePartialRowFrom(const Page *other, Row *dst_row, const Row *src_row,
                                  size_t x_start, size_t x_end_req);

    /* Get a single row. y must be valid. */
    Row *getRow(size_t y) const {
        /* assert(y < self.size.rows) */
        return &rows.ptr(memory)[y];
    }

    /* Get the cells for a row. Wisp: the pointer; the length is size.cols. */
    Cell *getCells(const Row *row) const {
        if (slow_runtime_safety) {
            /* assert(@intFromPtr(row) >= @intFromPtr(rows));
             * assert(@intFromPtr(row) < @intFromPtr(cells)) */
        }
        return row->cells().ptr(memory);
    }

    struct RowAndCell {
        Row *row;
        Cell *cell;
    };

    /* Get the row and cell for the given X/Y within this page. */
    RowAndCell getRowAndCell(size_t x, size_t y) const {
        /* assert(y < self.size.rows); assert(x < self.size.cols) */
        Row *rws = rows.ptr(memory);
        Row *row = &rws[y];
        Cell *cell = &row->cells().ptr(memory)[x];
        RowAndCell r;
        r.row = row;
        r.cell = cell;
        return r;
    }

    /* Move a cell from one location to another. This will replace the
     * previous contents with a blank cell. Because this is a move, this
     * doesn't allocate and can't fail. */
    void moveCells(Row *src_row, size_t src_left, Row *dst_row, size_t dst_left, size_t len);

    /* Swap two cells within the same row as quickly as possible. */
    void swapCells(Cell *src, Cell *dst);

    /* Clear the cells in the given row. This will reclaim memory used
     * by graphemes and styles. Note that if the style cleared is still
     * active, Page cannot know this and it will still be ref counted down.
     * The best solution for this is to artificially increment the ref count
     * prior to calling this function. */
    void clearCells(Row *row, size_t left, size_t end);

    /* Reset the given row to the default state: all cells zeroed and
     * all row metadata reset, as if the row was never used. This
     * reclaims memory used by graphemes, styles, etc. like clearCells.
     *
     * This must be used instead of clearCells whenever a row's storage
     * is recycled. Clearing the cells alone is not enough because row
     * metadata such as the wrap state and semantic prompt remain. */
    void resetRow(Row *row) {
        clearCells(row, 0, size.cols);
        row->reset();
    }

    /* Returns the hyperlink ID for the given cell. */
    bool lookupHyperlink(const Cell *cell, hyperlink::Id *out) const {
        const size::Offset<Cell> cell_offset = size::getOffset<Cell>((const void *)memory, cell);
        const hyperlink::Map::Unmanaged map = hyperlink_map.map((const void *)memory);
        return map.get(cell_offset, out);
    }

    /* Clear the hyperlink from the given cell.
     *
     * In order to update the hyperlink flag on the row, call
     * `updateRowHyperlinkFlag` after you finish clearing any
     * hyperlinks in the row. */
    void clearHyperlink(Cell *cell) {
        /* Get our ID */
        const size::Offset<Cell> cell_offset = size::getOffset<Cell>((const void *)memory, cell);
        hyperlink::Map::Unmanaged map = hyperlink_map.map((const void *)memory);
        hyperlink::Map::Unmanaged::Entry entry;
        if (!map.getEntry(cell_offset, &entry)) {
            assertIntegrity();
            return;
        }

        /* Release our usage of this, free memory, unset flag */
        hyperlink_set.release((const void *)memory, *entry.value_ptr);
        map.removeByPtr(entry.key_ptr);
        cell->setHyperlink(false);
        assertIntegrity();
    }

    /* Checks if the row contains any hyperlinks and sets
     * the hyperlink flag to false if none are found.
     *
     * Call after removing hyperlinks in a row. */
    void updateRowHyperlinkFlag(Row *row) {
        const Cell *c = row->cells().ptr(memory);
        for (size_t i = 0; i < size.cols; i++) if (c[i].hyperlink()) return;
        row->setHyperlink(false);
    }

    /* Convert a hyperlink into a page entry, returning the ID.
     *
     * This does not de-dupe any strings, so if the URI, explicit ID,
     * etc. is already in the strings table this will duplicate it.
     *
     * To release the memory associated with the given hyperlink,
     * release the ID from the `hyperlink_set`. If the refcount reaches
     * zero and the slot is needed then the context will reap the
     * memory. */
    PageError insertHyperlink(const hyperlink::Hyperlink &link, hyperlink::Id *out);

    /* Set the hyperlink for the given cell. If the cell already has a
     * hyperlink, then this will handle memory management and refcount
     * update for the prior hyperlink.
     *
     * DOES NOT increment the reference count for the new hyperlink!
     *
     * Caller is responsible for updating the refcount in the hyperlink
     * set as necessary by calling `use` if the id was not acquired with
     * `add`. */
    PageError setHyperlink(Row *row, Cell *cell, hyperlink::Id id);

    /* Move the hyperlink from one cell to another. This can't fail
     * because we avoid any allocations since we're just moving data.
     * Destination must NOT have a hyperlink. */
    void moveHyperlink(Cell *src, Cell *dst) {
        /* assert(src.hyperlink); assert(!dst.hyperlink) */
        const size::Offset<Cell> src_offset = size::getOffset<Cell>((const void *)memory, src);
        const size::Offset<Cell> dst_offset = size::getOffset<Cell>((const void *)memory, dst);
        hyperlink::Map::Unmanaged map = hyperlink_map.map((const void *)memory);
        hyperlink::Map::Unmanaged::Entry entry;
        map.getEntry(src_offset, &entry);
        const hyperlink::Id value = *entry.value_ptr;
        map.removeByPtr(entry.key_ptr);
        map.putAssumeCapacityNoClobber(dst_offset, value);

        /* NOTE: We must not set src/dst.hyperlink here because this
         * function is used in various cases where we swap cell contents
         * and its unsafe. The flip side: the caller must be careful
         * to set the proper cell state to represent the move. */
    }

    /* Returns the number of hyperlinks in the page. This isn't the byte
     * size but the total number of unique cells that have hyperlink data. */
    size_t hyperlinkCount() const { return hyperlink_map.map((const void *)memory).count(); }

    /* Returns the hyperlink capacity for the page. This isn't the byte
     * size but the number of unique cells that can have hyperlink data. */
    size_t hyperlinkCapacity() const { return hyperlink_map.map((const void *)memory).maxLoad(); }

    /* Set the graphemes for the given cell. This asserts that the cell
     * has no graphemes set, and only contains a single codepoint. Input
     * beyond grapheme_max_len is ignored. */
    PageError setGraphemes(Row *row, Cell *cell, const uint32_t *cps, size_t cps_len);

    /* Append a codepoint to the given cell as a grapheme. Once the cell has
     * grapheme_max_len suffix codepoints, additional codepoints are ignored.
     * Wisp: Allocator.Error — OutOfMemory or none. */
    PageError appendGrapheme(Row *row, Cell *cell, uint32_t cp);

    /* Returns the codepoints for the given cell. These are the codepoints
     * in addition to the first codepoint. The first codepoint is NOT
     * included since it is on the cell itself.
     * Wisp: ?[]u21 — null is nullptr; *len gets the length. */
    uint32_t *lookupGrapheme(const Cell *cell, size_t *len) const {
        const size::Offset<Cell> cell_offset = size::getOffset<Cell>((const void *)memory, cell);
        const GraphemeMap::Unmanaged map = grapheme_map.map((const void *)memory);
        size::Offset<uint32_t>::Slice slice;
        if (!map.get(cell_offset, &slice)) return nullptr;
        *len = slice.len;
        return slice.slice((const void *)memory);
    }

    /* Move the graphemes from one cell to another. This can't fail
     * because we avoid any allocations since we're just moving data.
     *
     * WARNING: This will NOT change the content_tag on the cells because
     * there are scenarios where we want to move graphemes without changing
     * the content tag. Callers beware but assertIntegrity should catch this. */
    void moveGrapheme(Cell *src, Cell *dst) {
        if (slow_runtime_safety) {
            /* assert(src.hasGrapheme()); assert(!dst.hasGrapheme()) */
        }

        const size::Offset<Cell> src_offset = size::getOffset<Cell>((const void *)memory, src);
        const size::Offset<Cell> dst_offset = size::getOffset<Cell>((const void *)memory, dst);
        GraphemeMap::Unmanaged map = grapheme_map.map((const void *)memory);
        GraphemeMap::Unmanaged::Entry entry;
        map.getEntry(src_offset, &entry);
        const size::Offset<uint32_t>::Slice value = *entry.value_ptr;
        map.removeByPtr(entry.key_ptr);
        map.putAssumeCapacityNoClobber(dst_offset, value);
    }

    /* Clear the graphemes for a given cell.
     *
     * In order to update the grapheme flag on the row, call
     * `updateRowGraphemeFlag` after you finish clearing any
     * graphemes in the row. */
    void clearGrapheme(Cell *cell) {
        /* assert(cell.hasGrapheme()) */

        /* Get our entry in the map, which must exist */
        const size::Offset<Cell> cell_offset = size::getOffset<Cell>((const void *)memory, cell);
        GraphemeMap::Unmanaged map = grapheme_map.map((const void *)memory);
        GraphemeMap::Unmanaged::Entry entry;
        map.getEntry(cell_offset, &entry);

        /* Free our grapheme data */
        const size::Offset<uint32_t>::Slice cps = *entry.value_ptr;
        grapheme_alloc.free((const void *)memory, cps.slice((const void *)memory), cps.len);

        /* Remove the entry */
        map.removeByPtr(entry.key_ptr);

        /* Mark that we no longer have graphemes by changing the content tag. */
        cell->setContentTag(Cell::ContentTag::codepoint);
        assertIntegrity();
    }

    /* Checks if the row contains any graphemes and sets
     * the grapheme flag to false if none are found.
     *
     * Call after removing graphemes in a row. */
    void updateRowGraphemeFlag(Row *row) {
        const Cell *c = row->cells().ptr(memory);
        for (size_t i = 0; i < size.cols; i++) if (c[i].hasGrapheme()) return;
        row->setGrapheme(false);
    }

    /* Returns the number of graphemes in the page. This isn't the byte
     * size but the total number of unique cells that have grapheme data. */
    size_t graphemeCount() const { return grapheme_map.map((const void *)memory).count(); }

    /* Returns the grapheme capacity for the page. This isn't the byte
     * size but the number of unique cells that can have grapheme data. */
    size_t graphemeCapacity() const { return grapheme_map.map((const void *)memory).capacity(); }

    /* Checks if the row contains any styles and sets
     * the styled flag to false if none are found.
     *
     * Call after removing styles in a row. */
    void updateRowStyledFlag(Row *row) {
        const Cell *c = row->cells().ptr(memory);
        for (size_t i = 0; i < size.cols; i++) if (c[i].hasStyling()) return;
        row->setStyled(false);
    }

    /* Returns true if this page is dirty at all. */
    bool isDirty() const {
        if (dirty) return true;
        const Row *r = rows.ptr(memory);
        for (size_t i = 0; i < size.rows; i++) {
            if (r[i].dirty()) return true;
        }
        return false;
    }
};

/* ─── Capacity (needs Page::layout) ──────────────────────────────────────── */

inline size_t Capacity::availableBytesForGrid() const {
    /* comptime assert(cells_align % Page.MetaLayout.alignment == 0);
     * comptime assert(@sizeOf(Cell) % Page.MetaLayout.alignment == 0) */
    const Page::Layout l = Page::layout(*this);
    return l.total_size - Page::MetaLayout::init(*this).total_size;
}

inline bool Capacity::maxCols(size::CellCountInt *out) const {
    const size_t available = availableBytesForGrid();

    /* A single row's header occupies a whole cell-aligned region
     * ahead of the cells. If we can't even fit that, return null. */
    const size_t row_region = alignForward(sizeof(Row), cells_align);
    if (available <= row_region) return false;

    /* We do the math of how many columns we can fit in the remaining
     * bytes ignoring the metadata of a row. */
    const size_t max_cols = (available - row_region) / sizeof(Cell);

    /* Clamp to CellCountInt max */
    *out = (size::CellCountInt)(max_cols < 0xFFFF ? max_cols : 0xFFFF);
    return true;
}

inline bool Capacity::adjust(Adjustment req, Capacity *out) const {
    Capacity adjusted = *this;
    if (req.has_cols) {
        const size::CellCountInt c = req.cols;
        const size_t total_size = Page::layout(*this).total_size;
        const size_t available = availableBytesForGrid();

        /* The size per row is:
         *   - The row metadata itself
         *   - The cells per row (n=cols) */
        const size_t bytes_per_row = sizeof(Row) + sizeof(Cell) * (size_t)c;
        size_t new_rows = available / bytes_per_row;

        /* The cell array is aligned to a cache line, so the padding
         * between the row headers and the cells depends on the row
         * count. Trim rows until the layout fits the original size.
         * The padding is less than a cache line so this takes a
         * handful of iterations at most. */
        adjusted.cols = c;
        while (new_rows > 0) {
            adjusted.rows = (size::CellCountInt)new_rows;
            if (Page::layout(adjusted).total_size <= total_size) break;
            new_rows -= 1;
        }

        /* If our rows go to zero then we can't fit any row metadata
         * for the desired number of columns. */
        if (new_rows == 0) return false;
    }

    *out = adjusted;
    return true;
}

/* ─── Page method bodies ─────────────────────────────────────────────────── */

inline PageError Page::verifyIntegrity() const {
    /* Some things that seem like we should check but do not:
     *
     * - We do not check that the style ref count is exact, only that
     *   it is at least what we see. We do this because some fast paths
     *   trim rows without clearing data.
     * - We do not check that styles seen is exactly the same as the
     *   styles count in the page for the same reason as above.
     * - We only check that we saw less graphemes than the total memory
     *   used for the same reason as styles above. */

    if (slow_runtime_safety) {
        if (pause_integrity_checks > 0) return PageError::none;
    }

    if (size.rows == 0) {
        /* log.warn("page integrity violation zero row count") */
        return PageError::ZeroRowCount;
    }
    if (size.cols == 0) {
        /* log.warn("page integrity violation zero col count") */
        return PageError::ZeroColCount;
    }

    size_t graphemes_seen = 0;
    std::unordered_map<style::Id, size_t> styles_seen;
    std::unordered_map<hyperlink::Id, size_t> hyperlinks_seen;

    const size_t grapheme_count = graphemeCount();

    const Row *rws = rows.ptr(memory);
    for (size_t y = 0; y < size.rows; y++) {
        const Row *row = &rws[y];
        const size_t graphemes_start = graphemes_seen;
        const Cell *cls = row->cells().ptr(memory);
        for (size_t x = 0; x < size.cols; x++) {
            const Cell *cell = &cls[x];
            size_t glen;
            if (cell->hasGrapheme()) {
                /* If a cell has grapheme data, it must be present in
                 * the grapheme map. */
                if (!lookupGrapheme(cell, &glen)) {
                    /* log.warn("page integrity violation y={} x={} grapheme data missing") */
                    return PageError::MissingGraphemeData;
                }

                graphemes_seen += 1;
            } else if (grapheme_count > 0) {
                /* It should not have grapheme data if it isn't marked.
                 * The grapheme_count check above is just an optimization
                 * to speed up integrity checks. */
                if (lookupGrapheme(cell, &glen) != nullptr) {
                    /* log.warn("page integrity violation y={} x={} cell not marked as grapheme") */
                    return PageError::UnmarkedGraphemeCell;
                }
            }

            if (cell->style_id() != style::default_id) {
                /* If a cell has a style, it must be present in the styles
                 * set. Accessing it with `get` asserts that. */
                (void)styles.get((const void *)memory, cell->style_id());

                if (!row->styled()) {
                    /* log.warn("page integrity violation y={} x={} row not marked as styled") */
                    return PageError::UnmarkedStyleRow;
                }

                styles_seen[cell->style_id()] += 1;
            }

            if (cell->hyperlink()) {
                hyperlink::Id id;
                if (!lookupHyperlink(cell, &id)) {
                    /* log.warn("page integrity violation y={} x={} hyperlink data missing") */
                    return PageError::MissingHyperlinkData;
                }

                if (!row->hyperlink()) {
                    /* log.warn("page integrity violation y={} x={} row not marked as hyperlink") */
                    return PageError::UnmarkedHyperlinkRow;
                }

                hyperlinks_seen[id] += 1;

                /* Hyperlink ID should be valid. This just straight crashes
                 * if this fails due to assertions. */
                (void)hyperlink_set.get((const void *)memory, id);
            } else {
                /* It should not have hyperlink data if it isn't marked */
                hyperlink::Id id;
                if (lookupHyperlink(cell, &id)) {
                    /* log.warn("page integrity violation y={} x={} cell not marked as hyperlink") */
                    return PageError::UnmarkedHyperlinkCell;
                }
            }

            switch (cell->wide()) {
                case Cell::Wide::narrow: break;
                case Cell::Wide::wide: break;

                case Cell::Wide::spacer_tail:
                    /* Spacer tails can't be at the start because they follow
                     * a wide char. */
                    if (x == 0) {
                        /* log.warn("page integrity violation y={} x={} spacer tail at start") */
                        return PageError::InvalidSpacerTailLocation;
                    }

                    /* Spacer tails must follow a wide char */
                    if (cls[x - 1].wide() != Cell::Wide::wide) {
                        /* log.warn("page integrity violation y={} x={} spacer tail not following wide") */
                        return PageError::InvalidSpacerTailLocation;
                    }
                    break;

                case Cell::Wide::spacer_head:
                    /* Spacer heads must be at the end */
                    if (x != (size_t)size.cols - 1) {
                        /* log.warn("page integrity violation y={} x={} spacer head not at end") */
                        return PageError::InvalidSpacerHeadLocation;
                    }

                    /* The row must be wrapped */
                    if (!row->wrap()) {
                        /* log.warn("page integrity violation y={} spacer head not wrapped") */
                        return PageError::UnwrappedSpacerHead;
                    }
                    break;
            }
        }

        /* Check row grapheme data */
        if (graphemes_seen > graphemes_start) {
            /* If a cell in a row has grapheme data, the row must
             * be marked as having grapheme data. */
            if (!row->grapheme()) {
                /* log.warn("page integrity violation y={} grapheme data but row not marked") */
                return PageError::UnmarkedGraphemeRow;
            }
        }
    }

    /* Our graphemes seen should exactly match the grapheme count */
    if (graphemes_seen > graphemeCount()) {
        /* log.warn("page integrity violation grapheme count mismatch") */
        return PageError::InvalidGraphemeCount;
    }

    /* Verify all our styles have the correct ref count. */
    for (auto it = styles_seen.begin(); it != styles_seen.end(); ++it) {
        const size::CellCountInt ref_count = styles.refCount((const void *)memory, it->first);
        if (ref_count < it->second) {
            /* log.warn("page integrity violation style ref count mismatch") */
            return PageError::MismatchedStyleRef;
        }
    }

    /* Verify all our hyperlinks have the correct ref count. */
    for (auto it = hyperlinks_seen.begin(); it != hyperlinks_seen.end(); ++it) {
        const size::CellCountInt ref_count = hyperlink_set.refCount((const void *)memory, it->first);
        if (ref_count < it->second) {
            /* log.warn("page integrity violation hyperlink ref count mismatch") */
            return PageError::MismatchedHyperlinkRef;
        }
    }

    /* Verify there are no zombie styles, that is, styles in the
     * set with ref counts > 0, which are not present in the page.
     *
     * NOTE: This is currently disabled because @qwerasd says that
     * certain fast paths can cause this but its okay.
     * Wisp: upstream still counts the zombies and then ignores the count;
     * the count has no effect and is not computed here. */

    return PageError::none;
}

inline Capacity Page::exactRowCapacity(size_t y_start, size_t y_end) const {
    /* assert(y_start < y_end); assert(y_end <= self.size.rows) */

    /* Track unique IDs using a bitset. Both style IDs and hyperlink IDs
     * are CellCountInt (u16), so we reuse this set for both to save
     * stack memory (~8KB instead of ~16KB).
     * Wisp: std.StaticBitSet(65536) as 1024 u64 words. */
    static const size_t words = 65536 / 64;
    uint64_t id_set[words];
    memset(id_set, 0, sizeof(id_set));
    size_t id_count = 0;
    #define WISP_IDSET_SET(v) do { const size_t v_ = (v); \
        if (!(id_set[v_ / 64] & ((uint64_t)1 << (v_ % 64)))) { id_set[v_ / 64] |= (uint64_t)1 << (v_ % 64); id_count++; } } while (0)
    #define WISP_IDSET_ISSET(v) ((id_set[(v) / 64] >> ((v) % 64)) & 1)

    /* Accumulators */
    size_t grapheme_bytes = 0;
    size_t string_bytes = 0;

    /* First pass: count styles and grapheme bytes */
    const Row *rws = rows.ptr(memory);
    for (size_t y = y_start; y < y_end; y++) {
        const Cell *cls = rws[y].cells().ptr(memory);
        for (size_t x = 0; x < size.cols; x++) {
            const Cell *cell = &cls[x];
            if (cell->style_id() != style::default_id) {
                WISP_IDSET_SET(cell->style_id());
            }

            if (cell->hasGrapheme()) {
                size_t n;
                if (lookupGrapheme(cell, &n)) {
                    grapheme_bytes += GraphemeAlloc::bytesRequired<uint32_t>(n);
                }
            }
        }
    }
    const size_t styles_cap = StyleSet::capacityForCount(id_count);

    /* Second pass: count hyperlinks and string bytes
     * We count both unique hyperlinks (for hyperlink_set) and total
     * hyperlink cells (for hyperlink_map capacity). */
    memset(id_set, 0, sizeof(id_set));
    id_count = 0;
    size_t hyperlink_cells = 0;
    for (size_t y = y_start; y < y_end; y++) {
        const Cell *cls = rws[y].cells().ptr(memory);
        for (size_t x = 0; x < size.cols; x++) {
            const Cell *cell = &cls[x];
            if (cell->hyperlink()) {
                hyperlink_cells += 1;
                hyperlink::Id id;
                if (lookupHyperlink(cell, &id)) {
                    /* Only count each unique hyperlink once for set sizing */
                    if (!WISP_IDSET_ISSET(id)) {
                        WISP_IDSET_SET(id);

                        /* Get the hyperlink entry to compute string bytes */
                        const hyperlink::PageEntry *entry = hyperlink_set.get((const void *)memory, id);
                        string_bytes += StringAlloc::bytesRequired<uint8_t>(entry->uri.len);

                        switch (entry->id.tag) {
                            case hyperlink::PageEntry::Id::Tag::implicit: break;
                            case hyperlink::PageEntry::Id::Tag::explicit_:
                                string_bytes += StringAlloc::bytesRequired<uint8_t>(entry->id.explicit_.len);
                                break;
                        }
                    }
                }
            }
        }
    }
    #undef WISP_IDSET_SET
    #undef WISP_IDSET_ISSET

    /* layout() requests `hyperlink_count * hyperlink_cell_multiplier`
     * usable map entries. The map layout adds load-factor headroom and
     * rounds the raw slot count to a power of two. We need enough
     * hyperlink_bytes for that requested entry count to accommodate all
     * hyperlink cells. This is unit tested. */
    const size_t hyperlink_set_cap = hyperlink::Set::capacityForCount(id_count);
    const size_t hyperlink_map_min = (hyperlink_cells + hyperlink_cell_multiplier - 1) / hyperlink_cell_multiplier;
    const size_t hyperlink_cap = hyperlink_set_cap > hyperlink_map_min ? hyperlink_set_cap : hyperlink_map_min;

    /* All the intCasts below are safe because we should have a
     * capacity strictly less than or equal to this page's capacity. */
    Capacity c;
    c.cols = size.cols;
    c.rows = (size::CellCountInt)(y_end - y_start);
    c.styles = (size::StyleCountInt)styles_cap;
    c.grapheme_bytes = (size::GraphemeBytesInt)grapheme_bytes;
    c.hyperlink_bytes = (size::HyperlinkCountInt)(hyperlink_cap * sizeof(hyperlink::Set::Item));
    c.string_bytes = (size::StringBytesInt)string_bytes;
    return c;
}

inline PageError Page::clonePartialRowFrom(const Page *other, Row *dst_row, const Row *src_row,
                                           size_t x_start, size_t x_end_req) {
    /* This whole operation breaks integrity until the end. */
    pauseIntegrityChecks(true);
    struct Resume {
        Page *p;
        ~Resume() {
            p->pauseIntegrityChecks(false);
            p->assertIntegrity();
        }
    } resume = { this };

    const size_t cell_len = size.cols < other->size.cols ? size.cols : other->size.cols;
    const size_t x_end = x_end_req < cell_len ? x_end_req : cell_len;
    /* assert(x_start <= x_end) */
    Cell *other_cells = src_row->cells().ptr(other->memory) + x_start;
    Cell *cls = dst_row->cells().ptr(memory) + x_start;
    const size_t n = x_end - x_start;

    /* If our destination has styles or graphemes then we need to
     * clear some state. This will free up the managed memory as well. */
    if (dst_row->managedMemory()) clearCells(dst_row, x_start, x_end);

    /* Copy all the row metadata but keep our cells offset */
    {
        Row copy = *src_row;

        /* If we're not copying the full row then we want to preserve
         * some original state from our dst row. */
        if (n < size.cols) {
            copy.setWrap(dst_row->wrap());
            copy.setWrapContinuation(dst_row->wrap_continuation());
            copy.setGrapheme(dst_row->grapheme());
            copy.setHyperlink(dst_row->hyperlink());
            copy.setStyled(dst_row->styled());
            copy.setDirty(copy.dirty() || dst_row->dirty());
        }

        /* Our cell offset remains the same */
        copy.setCells(dst_row->cells());

        *dst_row = copy;
    }

    /* If we have no managed memory in the source, then we can just
     * copy it directly. */
    if (!src_row->managedMemory()) {
        /* This is an integrity check: if the row claims it doesn't
         * have managed memory then all cells must also not have
         * managed memory. */
        fastmem::copy<Cell>(cls, other_cells, n);
    } else {
        /* We have managed memory, so we have to do a slower copy to
         * get all of that right. */
        for (size_t i = 0; i < n; i++) {
            Cell *dst_cell = &cls[i];
            const Cell *src_cell = &other_cells[i];
            *dst_cell = *src_cell;

            /* Reset any managed memory markers on the cell so that we don't
             * hit an integrity check if we have to return an error because
             * the page can't fit the new memory. */
            dst_cell->setHyperlink(false);
            dst_cell->setStyleId(style::default_id);
            if (dst_cell->content_tag() == Cell::ContentTag::codepoint_grapheme) {
                dst_cell->setContentTag(Cell::ContentTag::codepoint);
            }

            if (src_cell->hasGrapheme()) {
                /* Copy the grapheme codepoints */
                size_t cps_len;
                const uint32_t *cps = other->lookupGrapheme(src_cell, &cps_len);

                /* Safe to use setGraphemes because we cleared all
                 * managed memory for our destination cell range. */
                const PageError e = setGraphemes(dst_row, dst_cell, cps, cps_len);
                if (e != PageError::none) return e;
            }
            if (src_cell->hyperlink()) {
                hyperlink::Id id;
                other->lookupHyperlink(src_cell, &id);

                /* Fast-path: same page we can add with the same id. */
                if (other == this) {
                    hyperlink_set.use((const void *)memory, id);
                    const PageError e = setHyperlink(dst_row, dst_cell, id);
                    if (e != PageError::none) return e;
                } else {
                    /* Slow-path: get the hyperlink from the other page,
                     * add it, and migrate. */

                    /* If our page can't support an additional cell with
                     * a hyperlink then we have to return an error. */
                    if (hyperlinkCount() >= hyperlinkCapacity()) {
                        /* The hyperlink map capacity needs to be increased. */
                        return PageError::HyperlinkMapOutOfMemory;
                    }

                    const hyperlink::PageEntry *other_link = other->hyperlink_set.get((const void *)other->memory, id);
                    hyperlink::Id dst_id;
                    {
                        /* First check if the link already exists in our page,
                         * and increment its refcount if so, since we're about
                         * to use it. */
                        hyperlink::Id existing;
                        if (hyperlink_set.lookupContext((const void *)memory, *other_link,
                                                        hyperlink::SetContext(this, other), &existing)) {
                            hyperlink_set.use((const void *)memory, existing);
                            dst_id = existing;
                        } else {
                            /* If we don't have this link in our page yet then
                             * we need to clone it over and add it to our set. */

                            /* Clone the link. */
                            hyperlink::PageEntry dst_link;
                            if (!other_link->dupe(other, this, &dst_link)) {
                                /* The string alloc capacity needs to be increased. */
                                return PageError::StringAllocOutOfMemory;
                            }

                            /* Add it, preferring to use the same ID as the other
                             * page, since this *probably* speeds up full-page
                             * clones.
                             *
                             * TODO(qwerasd): verify the assumption that `addWithId`
                             * is ever actually useful, I think it may not be. */
                            bool is_null;
                            hyperlink::Id added;
                            const ref_counted_set::AddError e = hyperlink_set.addWithIdContext(
                                (const void *)memory, dst_link, id, hyperlink::SetContext(this), &is_null, &added);
                            switch (e) {
                                case ref_counted_set::AddError::none: break;
                                /* The hyperlink set capacity needs to be increased. */
                                case ref_counted_set::AddError::OutOfMemory: return PageError::HyperlinkSetOutOfMemory;
                                /* The hyperlink set needs to be rehashed. */
                                case ref_counted_set::AddError::NeedsRehash: return PageError::HyperlinkSetNeedsRehash;
                            }
                            dst_id = is_null ? id : added;
                        }
                    }

                    const PageError e = setHyperlink(dst_row, dst_cell, dst_id);
                    if (e != PageError::none) return e;
                }
            }
            if (src_cell->style_id() != style::default_id) {
                dst_row->setStyled(true);

                if (other == this) {
                    /* If it's the same page we don't have to worry about
                     * copying the style, we can use the style ID directly. */
                    dst_cell->setStyleId(src_cell->style_id());
                    styles.use((const void *)memory, dst_cell->style_id());
                } else {
                    /* Slow path: Get the style from the other
                     * page and add it to this page's style set. */
                    const style::Style *other_style = other->styles.get((const void *)other->memory, src_cell->style_id());
                    bool is_null;
                    style::Id added;
                    const ref_counted_set::AddError e = styles.addWithId(
                        (const void *)memory, *other_style, src_cell->style_id(), &is_null, &added);
                    switch (e) {
                        case ref_counted_set::AddError::none: break;
                        /* The style set capacity needs to be increased. */
                        case ref_counted_set::AddError::OutOfMemory: return PageError::StyleSetOutOfMemory;
                        /* The style set needs to be rehashed. */
                        case ref_counted_set::AddError::NeedsRehash: return PageError::StyleSetNeedsRehash;
                    }
                    dst_cell->setStyleId(is_null ? src_cell->style_id() : added);
                }
            }
            if (src_cell->codepoint() == kitty_placeholder) {
                dst_row->setKittyVirtualPlaceholder(true);
            }
        }
    }

    /* If we are growing columns, then we need to ensure spacer heads
     * are cleared. Wisp: `cells` here is the slice starting at x_start. */
    if (size.cols > other->size.cols) {
        Cell *last = &cls[other->size.cols - 1];
        if (last->wide() == Cell::Wide::spacer_head) {
            last->setWide(Cell::Wide::narrow);
        }
    }

    return PageError::none;
}

inline void Page::moveCells(Row *src_row, size_t src_left, Row *dst_row, size_t dst_left, size_t len) {
    Cell *src_cells = src_row->cells().ptr(memory) + src_left;
    Cell *dst_cells = dst_row->cells().ptr(memory) + dst_left;

    /* Clear our destination now matter what */
    clearCells(dst_row, dst_left, dst_left + len);

    /* If src has no managed memory, this is very fast. */
    if (!src_row->managedMemory()) {
        fastmem::copy<Cell>(dst_cells, src_cells, len);
    } else {
        /* Source has graphemes or hyperlinks... */
        for (size_t i = 0; i < len; i++) {
            Cell *src = &src_cells[i];
            Cell *dst = &dst_cells[i];
            *dst = *src;
            if (src->hasGrapheme()) {
                /* Required for moveGrapheme assertions */
                dst->setContentTag(Cell::ContentTag::codepoint);
                moveGrapheme(src, dst);
                src->setContentTag(Cell::ContentTag::codepoint);
                dst->setContentTag(Cell::ContentTag::codepoint_grapheme);
                dst_row->setGrapheme(true);
            }
            if (src->hyperlink()) {
                dst->setHyperlink(false);
                moveHyperlink(src, dst);
                dst->setHyperlink(true);
                dst_row->setHyperlink(true);
            }
            if (src->codepoint() == kitty_placeholder) {
                dst_row->setKittyVirtualPlaceholder(true);
            }
        }
    }

    /* The destination row has styles if any of the cells are styled */
    if (!dst_row->styled()) {
        bool styled = false;
        for (size_t i = 0; i < len; i++) {
            if (dst_cells[i].style_id() != style::default_id) {
                styled = true;
                break;
            }
        }
        dst_row->setStyled(styled);
    }

    /* Clear our source row now that the copy is complete. We can NOT
     * use clearCells here because clearCells will garbage collect our
     * styles and graphames but we moved them above.
     *
     * Zero the cells as u64s since empirically this seems
     * to be a bit faster than using @memset(src_cells, .{}) */
    memset(src_cells, 0, len * sizeof(Cell));
    if (len == size.cols) {
        src_row->setGrapheme(false);
        src_row->setHyperlink(false);
        src_row->setStyled(false);
        src_row->setKittyVirtualPlaceholder(false);
    }

    assertIntegrity();
}

inline void Page::swapCells(Cell *src, Cell *dst) {
    /* Graphemes are keyed by cell offset so we do have to move them.
     * We do this first so that all our grapheme state is correct. */
    if (src->hasGrapheme() || dst->hasGrapheme()) {
        if (src->hasGrapheme() && !dst->hasGrapheme()) {
            moveGrapheme(src, dst);
        } else if (!src->hasGrapheme() && dst->hasGrapheme()) {
            moveGrapheme(dst, src);
        } else {
            /* Both had graphemes, so we have to manually swap */
            const size::Offset<Cell> src_offset = size::getOffset<Cell>((const void *)memory, src);
            const size::Offset<Cell> dst_offset = size::getOffset<Cell>((const void *)memory, dst);
            GraphemeMap::Unmanaged map = grapheme_map.map((const void *)memory);
            GraphemeMap::Unmanaged::Entry src_entry, dst_entry;
            map.getEntry(src_offset, &src_entry);
            map.getEntry(dst_offset, &dst_entry);
            const size::Offset<uint32_t>::Slice src_value = *src_entry.value_ptr;
            const size::Offset<uint32_t>::Slice dst_value = *dst_entry.value_ptr;
            *src_entry.value_ptr = dst_value;
            *dst_entry.value_ptr = src_value;
        }
    }

    /* Hyperlinks are keyed by cell offset. */
    if (src->hyperlink() || dst->hyperlink()) {
        if (src->hyperlink() && !dst->hyperlink()) {
            moveHyperlink(src, dst);
        } else if (!src->hyperlink() && dst->hyperlink()) {
            moveHyperlink(dst, src);
        } else {
            /* Both had hyperlinks, so we have to manually swap */
            const size::Offset<Cell> src_offset = size::getOffset<Cell>((const void *)memory, src);
            const size::Offset<Cell> dst_offset = size::getOffset<Cell>((const void *)memory, dst);
            hyperlink::Map::Unmanaged map = hyperlink_map.map((const void *)memory);
            hyperlink::Map::Unmanaged::Entry src_entry, dst_entry;
            map.getEntry(src_offset, &src_entry);
            map.getEntry(dst_offset, &dst_entry);
            const hyperlink::Id src_value = *src_entry.value_ptr;
            const hyperlink::Id dst_value = *dst_entry.value_ptr;
            *src_entry.value_ptr = dst_value;
            *dst_entry.value_ptr = src_value;
        }
    }

    /* Copy the metadata. Note that we do NOT have to worry about
     * styles because styles are keyed by ID and we're preserving the
     * exact ref count and row state here. */
    const Cell old_dst = *dst;
    *dst = *src;
    *src = old_dst;

    assertIntegrity();
}

inline void Page::clearCells(Row *row, size_t left, size_t end) {
    Cell *cls = row->cells().ptr(memory) + left;
    const size_t n = end - left;

    /* If we have managed memory (styles, graphemes, or hyperlinks)
     * in this row then we go cell by cell and clear them if present. */
    if (row->grapheme()) {
        for (size_t i = 0; i < n; i++) {
            if (cls[i].hasGrapheme()) clearGrapheme(&cls[i]);
        }

        /* If we have no left/right scroll region we can be sure
         * that we've cleared all the graphemes, so we clear the
         * flag, otherwise we use the update function to update. */
        if (n == size.cols) {
            row->setGrapheme(false);
        } else {
            updateRowGraphemeFlag(row);
        }
    }

    if (row->hyperlink()) {
        for (size_t i = 0; i < n; i++) {
            if (cls[i].hyperlink()) clearHyperlink(&cls[i]);
        }

        /* If we have no left/right scroll region we can be sure
         * that we've cleared all the hyperlinks, so we clear the
         * flag, otherwise we use the update function to update. */
        if (n == size.cols) {
            row->setHyperlink(false);
        } else {
            updateRowHyperlinkFlag(row);
        }
    }

    if (row->styled()) {
        for (size_t i = 0; i < n; i++) {
            if (cls[i].hasStyling()) styles.release((const void *)memory, cls[i].style_id());
        }

        /* If we have no left/right scroll region we can be sure
         * that we've cleared all the styles, so we clear the
         * flag, otherwise we use the update function to update. */
        if (n == size.cols) {
            row->setStyled(false);
        } else {
            updateRowStyledFlag(row);
        }
    }

    if (row->kitty_virtual_placeholder() && n == size.cols) {
        bool found = false;
        for (size_t i = 0; i < n; i++) {
            if (cls[i].codepoint() == kitty_placeholder) {
                found = true;
                break;
            }
        }
        if (!found) row->setKittyVirtualPlaceholder(false);
    }

    /* Zero the cells as u64s since empirically this seems
     * to be a bit faster than using @memset(cells, .{}) */
    memset(cls, 0, n * sizeof(Cell));

    assertIntegrity();
}

inline PageError Page::insertHyperlink(const hyperlink::Hyperlink &link, hyperlink::Id *out) {
    const void *base = memory;

    /* Insert our URI into the page strings table. */
    hyperlink::StrSlice page_uri;
    {
        uint8_t *buf;
        if (!string_alloc.alloc<uint8_t>(base, link.uri_len, &buf)) return PageError::StringsOutOfMemory;
        memcpy(buf, link.uri, link.uri_len);

        page_uri.offset = size::getOffset<uint8_t>(base, &buf[0]);
        page_uri.len = link.uri_len;
    }

    /* Allocate an ID for our page memory if we have to. */
    hyperlink::PageEntry::Id page_id;
    switch (link.id.tag) {
        case hyperlink::Hyperlink::Id::Tag::explicit_: {
            uint8_t *buf;
            if (!string_alloc.alloc<uint8_t>(base, link.id.explicit_len, &buf)) {
                string_alloc.free(base, page_uri.slice(base), page_uri.len);
                return PageError::StringsOutOfMemory;
            }
            memcpy(buf, link.id.explicit_ptr, link.id.explicit_len);

            hyperlink::StrSlice s;
            s.offset = size::getOffset<uint8_t>(base, &buf[0]);
            s.len = link.id.explicit_len;
            page_id = hyperlink::PageEntry::Id::makeExplicit(s);
            break;
        }

        case hyperlink::Hyperlink::Id::Tag::implicit:
            page_id = hyperlink::PageEntry::Id::makeImplicit(link.id.implicit);
            break;
    }

    /* Build our entry */
    hyperlink::PageEntry entry;
    entry.id = page_id;
    entry.uri = page_uri;

    /* Put our hyperlink into the hyperlink set to get an ID */
    hyperlink::Id id;
    const ref_counted_set::AddError e = hyperlink_set.addContext(base, entry, hyperlink::SetContext(this), &id);
    if (e != ref_counted_set::AddError::none) {
        /* errdefer: free the ID and URI strings */
        if (page_id.tag == hyperlink::PageEntry::Id::Tag::explicit_) {
            string_alloc.free(base, page_id.explicit_.slice(base), page_id.explicit_.len);
        }
        string_alloc.free(base, page_uri.slice(base), page_uri.len);
        return e == ref_counted_set::AddError::OutOfMemory ? PageError::SetOutOfMemory
                                                           : PageError::SetNeedsRehash;
    }

    *out = id;
    return PageError::none;
}

inline PageError Page::setHyperlink(Row *row, Cell *cell, hyperlink::Id id) {
    const size::Offset<Cell> cell_offset = size::getOffset<Cell>((const void *)memory, cell);
    hyperlink::Map::Unmanaged map = hyperlink_map.map((const void *)memory);
    hyperlink::Map::Unmanaged::GetOrPutResult gop;
    if (!map.getOrPut(cell_offset, &gop)) {
        /* The hyperlink map capacity needs to be increased. */
        assertIntegrity();
        return PageError::HyperlinkMapOutOfMemory;
    }

    if (gop.found_existing) {
        /* Always release the old hyperlink, because even if it's actually
         * the same as the one we're setting, we'd end up double-counting
         * if we left the reference count be, because the caller does not
         * know whether it's the same and will have increased the count
         * outside of this function. */
        hyperlink_set.release((const void *)memory, *gop.value_ptr);

        /* If the hyperlink matches then we don't need to do anything. */
        if (*gop.value_ptr == id) {
            /* It is possible for cell hyperlink to be false but row
             * must never be false. The cell hyperlink can be false because
             * in Terminal.print we clear the hyperlink for the cursor cell
             * before writing the cell again, so if someone prints over
             * a cell with a matching hyperlink this state can happen.
             * This is tested in Terminal.zig.
             * assert(row.hyperlink) */
            cell->setHyperlink(true);
            assertIntegrity();
            return PageError::none;
        }
    }

    /* Set the hyperlink on the cell and in the map. */
    *gop.value_ptr = id;
    cell->setHyperlink(true);
    row->setHyperlink(true);
    assertIntegrity();
    return PageError::none;
}

inline PageError Page::setGraphemes(Row *row, Cell *cell, const uint32_t *cps, size_t cps_len) {
    /* assert(cell.codepoint() > 0); assert(cell.content_tag == .codepoint) */
    const void *base = memory;

    const size::Offset<Cell> cell_offset = size::getOffset<Cell>(base, cell);
    GraphemeMap::Unmanaged map = grapheme_map.map(base);
    const size_t stored_len = cps_len < grapheme_max_len ? cps_len : grapheme_max_len;

    uint32_t *slice;
    if (!grapheme_alloc.alloc<uint32_t>(base, stored_len, &slice)) {
        /* The grapheme alloc capacity needs to be increased. */
        assertIntegrity();
        return PageError::GraphemeAllocOutOfMemory;
    }
    memcpy(slice, cps, stored_len * sizeof(uint32_t));

    size::Offset<uint32_t>::Slice v;
    v.offset = size::getOffset<uint32_t>(base, slice);
    v.len = stored_len;
    if (!map.putNoClobber(cell_offset, v)) {
        grapheme_alloc.free(base, slice, stored_len);
        /* The grapheme map capacity needs to be increased. */
        assertIntegrity();
        return PageError::GraphemeMapOutOfMemory;
    }

    cell->setContentTag(Cell::ContentTag::codepoint_grapheme);
    row->setGrapheme(true);

    assertIntegrity();
    return PageError::none;
}

inline PageError Page::appendGrapheme(Row *row, Cell *cell, uint32_t cp) {
    const void *base = memory;

    /* if (build_options.slow_runtime_safety) assert(cell.codepoint() != 0) */

    const size::Offset<Cell> cell_offset = size::getOffset<Cell>(base, cell);
    GraphemeMap::Unmanaged map = grapheme_map.map(base);

    /* If this cell has no graphemes, we can go faster by knowing we
     * need to allocate a new grapheme slice and update the map. */
    if (cell->content_tag() != Cell::ContentTag::codepoint_grapheme) {
        uint32_t *cps;
        if (!grapheme_alloc.alloc<uint32_t>(base, 1, &cps)) {
            assertIntegrity();
            return PageError::OutOfMemory;
        }
        cps[0] = cp;

        size::Offset<uint32_t>::Slice v;
        v.offset = size::getOffset<uint32_t>(base, cps);
        v.len = 1;
        if (!map.putNoClobber(cell_offset, v)) {
            grapheme_alloc.free(base, cps, 1);
            assertIntegrity();
            return PageError::OutOfMemory;
        }

        cell->setContentTag(Cell::ContentTag::codepoint_grapheme);
        row->setGrapheme(true);

        assertIntegrity();
        return PageError::none;
    }

    /* The cell already has graphemes. We need to append to the existing
     * grapheme slice and update the map.
     * assert(row.grapheme) */

    size::Offset<uint32_t>::Slice *slice = map.getPtr(cell_offset);

    /* Terminal input is untrusted. In addition to bounding memory, this
     * prevents repeated chunk growth and copying from becoming quadratic. */
    if (slice->len >= grapheme_max_len) {
        assertIntegrity();
        return PageError::none;
    }

    /* If our slice len doesn't divide evenly by the grapheme chunk
     * length then we can utilize the additional chunk space. */
    if (slice->len % grapheme_chunk_len != 0) {
        uint32_t *cps = slice->offset.ptr(base);
        cps[slice->len] = cp;
        slice->len += 1;
        assertIntegrity();
        return PageError::none;
    }

    /* We are out of chunk space. There is no fast path here. We need
     * to allocate a larger chunk. This is a very slow path. We expect
     * most graphemes to fit within our chunk size. */
    uint32_t *cps;
    if (!grapheme_alloc.alloc<uint32_t>(base, slice->len + 1, &cps)) {
        assertIntegrity();
        return PageError::OutOfMemory;
    }
    uint32_t *old_cps = slice->slice(base);
    const size_t old_len = slice->len;
    fastmem::copy<uint32_t>(cps, old_cps, old_len);
    cps[slice->len] = cp;
    slice->offset = size::getOffset<uint32_t>(base, cps);
    slice->len = old_len + 1;

    /* Free our old chunk */
    grapheme_alloc.free(base, old_cps, old_len);

    assertIntegrity();
    return PageError::none;
}

} /* namespace page */

/* ─── hyperlink.zig bodies that need Page ────────────────────────────────── */

namespace hyperlink {

inline bool PageEntry::dupe(const page::Page *self_page, page::Page *dst_page, PageEntry *out) const {
    PageEntry copy = *this;

    /* If the pages are the same then we can return a shallow copy. */
    if (self_page == dst_page) {
        *out = copy;
        return true;
    }

    const void *src_base = self_page->memory;
    const void *dst_base = dst_page->memory;

    /* Copy the URI */
    {
        const uint8_t *u = uri.slice(src_base);
        uint8_t *buf;
        if (!dst_page->string_alloc.alloc<uint8_t>(dst_base, uri.len, &buf)) return false;
        memcpy(buf, u, uri.len);
        copy.uri.offset = size::getOffset<uint8_t>(dst_base, &buf[0]);
        copy.uri.len = uri.len;
    }

    /* Copy the ID */
    switch (copy.id.tag) {
        case PageEntry::Id::Tag::implicit: break; /* Shallow is fine */
        case PageEntry::Id::Tag::explicit_: {
            const StrSlice slice = copy.id.explicit_;
            const uint8_t *idp = slice.slice(src_base);
            uint8_t *buf;
            if (!dst_page->string_alloc.alloc<uint8_t>(dst_base, slice.len, &buf)) {
                dst_page->string_alloc.free(dst_base, copy.uri.slice(dst_base), copy.uri.len);
                return false;
            }
            memcpy(buf, idp, slice.len);
            StrSlice s;
            s.offset = size::getOffset<uint8_t>(dst_base, &buf[0]);
            s.len = slice.len;
            copy.id = PageEntry::Id::makeExplicit(s);
            break;
        }
    }

    *out = copy;
    return true;
}

inline void PageEntry::free_(page::Page *pg) const {
    const void *base = pg->memory;
    switch (id.tag) {
        case Id::Tag::implicit: break;
        case Id::Tag::explicit_:
            if (id.explicit_.len > 0) pg->string_alloc.free(base, id.explicit_.slice(base), id.explicit_.len);
            break;
    }
    if (uri.len > 0) pg->string_alloc.free(base, uri.slice(base), uri.len);
}

inline uint64_t SetContext::hash(const PageEntry &link) const {
    return link.hash((const void *)(src_page ? src_page->memory : page->memory));
}

inline bool SetContext::eql(const PageEntry &a, const PageEntry &b) const {
    return a.eql((const void *)(src_page ? src_page->memory : page->memory), b, (const void *)page->memory);
}

inline void SetContext::deleted(const PageEntry &link) const { link.free_(page); }

} /* namespace hyperlink */

/* ─── style.zig bodies that need Cell ────────────────────────────────────── */

namespace style {

inline bool Style::bg(const page::Cell *cell, const Palette *palette, RGB *out) const {
    switch (cell->content_tag()) {
        case page::Cell::ContentTag::bg_color_palette:
            *out = palette->colors[cell->contentColorPalette()];
            return true;
        case page::Cell::ContentTag::bg_color_rgb: {
            const page::Cell::RGB rgb = cell->contentColorRgb();
            *out = RGB(rgb.r, rgb.g, rgb.b);
            return true;
        }

        default:
            switch (bg_color.tag) {
                case Color::Tag::none: return false;
                case Color::Tag::palette: *out = palette->colors[bg_color.palette]; return true;
                case Color::Tag::rgb: *out = bg_color.rgb; return true;
            }
    }
    return false;
}

inline bool Style::bgCell(page::Cell *out) const {
    switch (bg_color.tag) {
        case Color::Tag::none: return false;
        case Color::Tag::palette: {
            page::Cell c;
            c.setContentTag(page::Cell::ContentTag::bg_color_palette);
            c.setContentColorPalette(bg_color.palette);
            *out = c;
            return true;
        }
        case Color::Tag::rgb: {
            page::Cell c;
            c.setContentTag(page::Cell::ContentTag::bg_color_rgb);
            page::Cell::RGB rgb;
            rgb.r = bg_color.rgb.r;
            rgb.g = bg_color.rgb.g;
            rgb.b = bg_color.rgb.b;
            c.setContentColorRgb(rgb);
            *out = c;
            return true;
        }
    }
    return false;
}

} /* namespace style */

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_PAGE_HPP */
