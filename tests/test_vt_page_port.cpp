/* Transliterated from the test blocks in Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 *
 * Mapping:
 *   try Page.init(.{...})          PageHolder p(...)  (deinit on scope exit)
 *   rac.cell.* = .{ .content_tag = .codepoint, .content = .{ .codepoint =
 *     .{ .data = x } } }           *rac.cell = cp(x)
 *   rac.cell.content.codepoint.data   rac.cell->contentCodepoint()
 *   expectError(E, f)              ASSERT_TRUE(f == PageError::E)
 */

#include "test_helpers.h"
#include "../vt/page.hpp"

using namespace wisp::vt;
using namespace wisp::vt::page;
typedef style::Style Style;

struct PageHolder {
    Page p;
    bool ok;
    explicit PageHolder(const Capacity &cap) { ok = Page::init(cap, &p); }
    ~PageHolder() { if (ok) p.deinit(); }
    Page *operator->() { return &p; }
    Page &operator*() { return p; }
};

static Capacity cap(size::CellCountInt cols, size::CellCountInt rows, size::StyleCountInt styles = 16) {
    Capacity c(cols, rows);
    c.styles = styles;
    return c;
}

/* `.{ .content_tag = .codepoint, .content = .{ .codepoint = .{ .data = v } } }` */
static Cell cp(uint32_t v) {
    Cell c;
    c.setContentTag(Cell::ContentTag::codepoint);
    c.setContentCodepoint(v);
    return c;
}

static bool graphemeIs(const Page &page, const Cell *cell, const uint32_t *want, size_t n) {
    size_t len;
    const uint32_t *cps = page.lookupGrapheme(cell, &len);
    if (!cps || len != n) return false;
    return memcmp(cps, want, n * sizeof(uint32_t)) == 0;
}

static Style boldStyle() {
    Style s;
    s.flags.bold = true;
    return s;
}

static style::Id addStyle(Page &page, const Style &s) {
    style::Id id = 0;
    const ref_counted_set::AddError e = page.styles.add((const void *)page.memory, s, &id);
    (void)e;
    return id;
}

static hyperlink::Hyperlink link(const char *uri, bool explicit_id, const char *id_str, uint32_t implicit) {
    hyperlink::Hyperlink l;
    l.uri = (const uint8_t *)uri;
    l.uri_len = strlen(uri);
    l.id = explicit_id ? hyperlink::Hyperlink::Id::makeExplicit((const uint8_t *)id_str, strlen(id_str))
                       : hyperlink::Hyperlink::Id::makeImplicit(implicit);
    return l;
}

/* ─── Mask ─────────────────────────────────────────────────────────────── */

TEST(page, Mask) {
    typedef Mask<Cell, 4> M4;
    const M4 M(fields::cell_content_tag() | fields::cell_style_id());

    const Cell plain = Cell::init('A');
    Cell styled = Cell::init('B');
    styled.setStyleId(5);
    Cell styled2 = Cell::init('C');
    styled2.setStyleId(5);
    Cell other = Cell::init('D');
    other.setStyleId(6);

    /* match: plain cells only */
    {
        Cell cells[4] = { plain, plain, plain, plain };
        ASSERT_TRUE(M.match(cells, 0));
        ASSERT_TRUE(M.matchScalar(plain));

        cells[2] = styled;
        ASSERT_FALSE(M.match(cells, 0));
        ASSERT_FALSE(M.matchScalar(styled));
    }

    /* eql: runs of matching masked fields, other fields may vary */
    {
        const uint64_t expected = M.pattern(styled);
        Cell cells[4] = { styled, styled2, styled, styled2 };
        ASSERT_TRUE(M.eql(cells, 0, expected));
        ASSERT_TRUE(M.eqlScalar(styled2, expected));

        cells[1] = other;
        ASSERT_FALSE(M.eql(cells, 0, expected));
        ASSERT_FALSE(M.eqlScalar(other, expected));
    }

    /* eqlPrefix: count of leading values matching the pattern */
    {
        const uint64_t expected = M.pattern(styled);
        Cell cells[4] = { styled, styled2, other, styled };
        ASSERT_TRUE(M.eqlPrefix(cells, 0, expected) == 2);

        cells[2] = styled;
        ASSERT_TRUE(M.eqlPrefix(cells, 0, expected) == 4);
    }

    /* eqlExact: bit-identical values only */
    {
        const uint64_t expected = M4::bits(styled);
        Cell cells[4] = { styled, styled, styled, styled };
        ASSERT_TRUE(M.eqlExact(cells, 0, expected));

        /* Same masked fields but different codepoint is not exact. */
        cells[3] = styled2;
        ASSERT_FALSE(M.eqlExact(cells, 0, expected));
    }

    /* strip: compare values while ignoring the masked fields */
    {
        Cell styled_other = Cell::init('B');
        styled_other.setStyleId(6);
        ASSERT_TRUE(M.strip(styled) == M.strip(styled_other));
        ASSERT_TRUE(M.strip(styled) != M.strip(styled2));
    }

    /* eqlAny: presence of a matching value anywhere in the group */
    {
        const uint64_t expected = M.pattern(styled);
        Cell cells[4] = { plain, plain, plain, plain };
        ASSERT_FALSE(M.eqlAny(cells, 0, expected));

        cells[2] = styled;
        ASSERT_TRUE(M.eqlAny(cells, 0, expected));

        /* Masked compare: same masked fields with a different
         * codepoint still matches. */
        cells[2] = styled2;
        ASSERT_TRUE(M.eqlAny(cells, 0, expected));
    }
}

TEST(page, Mask_nested_field_path) {
    /* Mask only the codepoint data bits of the content field, not
     * the padding next to it or any other field. */
    const Mask<Cell, 4> M(fields::cell_content_codepoint_data());

    const Cell a = Cell::init('A');
    Cell b = Cell::init('A');
    b.setStyleId(5);
    b.setWide(Cell::Wide::wide);
    const Cell c = Cell::init('C');

    /* Same codepoint matches regardless of other fields. */
    const uint64_t expected = M.pattern(a);
    ASSERT_TRUE(M.eqlScalar(b, expected));
    ASSERT_FALSE(M.eqlScalar(c, expected));

    /* The mask must cover exactly the codepoint data bits. */
    const unsigned cp_offset = Cell::content_off;
    ASSERT_TRUE(((uint64_t)0x1FFFFF << cp_offset) == fields::cell_content_codepoint_data());

    /* Group variants */
    {
        Cell cells[4] = { a, b, a, b };
        ASSERT_TRUE(M.eql(cells, 0, expected));
        ASSERT_TRUE(M.eqlAny(cells, 0, expected));

        cells[1] = c;
        ASSERT_FALSE(M.eql(cells, 0, expected));
        ASSERT_TRUE(M.eqlAny(cells, 0, expected));

        const Cell none[4] = { c, c, c, c };
        ASSERT_FALSE(M.eqlAny(none, 0, expected));
    }
}

/* ─── layout / capacity ────────────────────────────────────────────────── */

TEST(page, Page_layout_can_take_a_maxed_capacity) {
    /* Our intention is for a maxed-out capacity to always fit
     * within a page layout without triggering runtime safety on any
     * overflow. This simplifies some of our handling downstream of the
     * call (relevant to: https://github.com/ghostty-org/ghostty/issues/10258) */
    Capacity c;
    c.cols = 0xFFFF;
    c.rows = 0xFFFF;
    c.styles = 0xFFFF;
    c.hyperlink_bytes = 0xFFFF;
    c.grapheme_bytes = 0xFFFFFFFFu;
    c.string_bytes = 0xFFFFFFFFu;

    /* Note that a max capacity will exceed our max_page_size so we
     * can't init a page with it, but it should layout. */
    (void)Page::layout(c);
}

TEST(page, Cell_is_zero_by_default) {
    const Cell cell = Cell::init(0);
    ASSERT_TRUE(cell.bits == 0);

    /* The zero value should be output type for semantic content.
     * This is very important for our assumptions elsewhere. */
    ASSERT_TRUE(cell.semantic_content() == Cell::SemanticContent::output);
}

static void adjustCase(size::CellCountInt cols) {
    const Capacity original = std_capacity();
    const size_t original_size = Page::layout(original).total_size;
    Capacity adjusted;
    ASSERT_TRUE(original.adjust(Capacity::Adjustment::withCols(cols), &adjusted));
    const size_t adjusted_size = Page::layout(adjusted).total_size;
    ASSERT_TRUE(original_size == adjusted_size);
    /* If we layout a page with 1 more row and it's still the same size
     * then adjust is not producing enough rows. */
    Capacity bigger = adjusted;
    bigger.rows += 1;
    const size_t bigger_size = Page::layout(bigger).total_size;
    ASSERT_TRUE(bigger_size > original_size);
}

TEST(page, Page_capacity_adjust_cols_down) { adjustCase(std_capacity().cols / 2); }
TEST(page, Page_capacity_adjust_cols_down_to_1) { adjustCase(1); }
TEST(page, Page_capacity_adjust_cols_up) { adjustCase(std_capacity().cols * 2); }

TEST(page, Page_capacity_adjust_cols_sweep) {
    Capacity c = std_capacity();
    const size::CellCountInt original_cols = c.cols;
    const size_t original_size = Page::layout(c).total_size;
    for (size_t col = 1; col < (size_t)original_cols * 2; col++) {
        ASSERT_TRUE(c.adjust(Capacity::Adjustment::withCols((size::CellCountInt)col), &c));
        const size_t adjusted_size = Page::layout(c).total_size;
        ASSERT_TRUE(original_size == adjusted_size);
        Capacity bigger = c;
        bigger.rows += 1;
        const size_t bigger_size = Page::layout(bigger).total_size;
        ASSERT_TRUE(bigger_size > original_size);
    }
}

TEST(page, Page_capacity_adjust_cols_too_high) {
    const Capacity original = std_capacity();
    Capacity out;
    ASSERT_FALSE(original.adjust(Capacity::Adjustment::withCols(0xFFFF), &out)); /* error.OutOfMemory */
}

TEST(page, Capacity_maxCols_basic) {
    const Capacity c = std_capacity();
    size::CellCountInt max;
    ASSERT_TRUE(c.maxCols(&max));

    /* maxCols should be >= current cols (since current capacity is valid) */
    ASSERT_TRUE(max >= c.cols);

    /* Adjusting to maxCols should succeed with at least 1 row */
    Capacity adjusted;
    ASSERT_TRUE(c.adjust(Capacity::Adjustment::withCols(max), &adjusted));
    ASSERT_TRUE(adjusted.rows >= 1);

    /* Adjusting to maxCols + 1 should fail */
    Capacity out;
    ASSERT_FALSE(c.adjust(Capacity::Adjustment::withCols((size::CellCountInt)(max + 1)), &out));
}

TEST(page, Capacity_maxCols_preserves_total_size) {
    const Capacity c = std_capacity();
    const size_t original_size = Page::layout(c).total_size;
    size::CellCountInt max;
    ASSERT_TRUE(c.maxCols(&max));
    Capacity adjusted;
    ASSERT_TRUE(c.adjust(Capacity::Adjustment::withCols(max), &adjusted));
    ASSERT_TRUE(original_size == Page::layout(adjusted).total_size);
}

TEST(page, Capacity_maxCols_with_1_row_exactly) {
    const Capacity c = std_capacity();
    size::CellCountInt max;
    ASSERT_TRUE(c.maxCols(&max));
    Capacity adjusted;
    ASSERT_TRUE(c.adjust(Capacity::Adjustment::withCols(max), &adjusted));
    ASSERT_TRUE(adjusted.rows == 1);
}

/* ─── basics ───────────────────────────────────────────────────────────── */

TEST(page, Page_init) {
    PageHolder page(cap(120, 80, 32));
    ASSERT_TRUE(page.ok);
}

TEST(page, Page_read_and_write_cells) {
    PageHolder page(cap(10, 10, 8));

    for (size_t y = 0; y < page->capacity.rows; y++) {
        *page->getRowAndCell(1, y).cell = cp((uint32_t)y);
    }

    /* Read it again */
    for (size_t y = 0; y < page->capacity.rows; y++) {
        ASSERT_TRUE(page->getRowAndCell(1, y).cell->contentCodepoint() == y);
    }
}

TEST(page, Page_appendGrapheme_small) {
    PageHolder page(cap(10, 10, 8));

    const Page::RowAndCell rac = page->getRowAndCell(0, 0);
    *rac.cell = Cell::init(0x09);

    /* One */
    ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    ASSERT_TRUE(rac.row->grapheme());
    ASSERT_TRUE(rac.cell->hasGrapheme());
    { const uint32_t w[] = { 0x0A }; ASSERT_TRUE(graphemeIs(*page, rac.cell, w, 1)); }

    /* Two */
    ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0B) == PageError::none);
    ASSERT_TRUE(rac.row->grapheme());
    ASSERT_TRUE(rac.cell->hasGrapheme());
    { const uint32_t w[] = { 0x0A, 0x0B }; ASSERT_TRUE(graphemeIs(*page, rac.cell, w, 2)); }

    /* Clear it */
    page->clearGrapheme(rac.cell);
    page->updateRowGraphemeFlag(rac.row);
    ASSERT_FALSE(rac.row->grapheme());
    ASSERT_FALSE(rac.cell->hasGrapheme());
}

TEST(page, Page_appendGrapheme_larger_than_chunk) {
    PageHolder page(cap(10, 10, 8));

    const Page::RowAndCell rac = page->getRowAndCell(0, 0);
    *rac.cell = Cell::init(0x09);

    const size_t count = grapheme_chunk_len * 10;
    for (size_t i = 0; i < count; i++) {
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, (uint32_t)(0x0A + i)) == PageError::none);
    }

    size_t len;
    const uint32_t *cps = page->lookupGrapheme(rac.cell, &len);
    ASSERT_TRUE(cps && len == count);
    for (size_t i = 0; i < count; i++) ASSERT_TRUE(cps[i] == 0x0A + i);
}

TEST(page, Page_appendGrapheme_caps_codepoints_per_cell) {
    PageHolder page(cap(10, 10, 8));

    const Page::RowAndCell rac = page->getRowAndCell(0, 0);
    *rac.cell = Cell::init('A');

    for (size_t i = 0; i < grapheme_max_len + 16; i++) {
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, (uint32_t)(0x0300 + i)) == PageError::none);
    }

    size_t len;
    const uint32_t *cps = page->lookupGrapheme(rac.cell, &len);
    ASSERT_TRUE(cps && len == grapheme_max_len);
    for (size_t i = 0; i < grapheme_max_len; i++) ASSERT_TRUE(cps[i] == 0x0300 + i);
}

TEST(page, Page_setGraphemes_caps_codepoints_per_cell) {
    PageHolder page(cap(10, 10, 8));

    uint32_t input[grapheme_max_len + 16];
    for (size_t i = 0; i < grapheme_max_len + 16; i++) input[i] = (uint32_t)(0x0300 + i);

    const Page::RowAndCell rac = page->getRowAndCell(0, 0);
    *rac.cell = Cell::init('A');
    ASSERT_TRUE(page->setGraphemes(rac.row, rac.cell, input, grapheme_max_len + 16) == PageError::none);

    size_t len;
    ASSERT_TRUE(page->lookupGrapheme(rac.cell, &len) && len == grapheme_max_len);
}

TEST(page, Page_clearGrapheme_not_all_cells) {
    PageHolder page(cap(10, 10, 8));

    const Page::RowAndCell rac = page->getRowAndCell(0, 0);
    *rac.cell = Cell::init(0x09);
    ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);

    const Page::RowAndCell rac2 = page->getRowAndCell(1, 0);
    *rac2.cell = Cell::init(0x09);
    ASSERT_TRUE(page->appendGrapheme(rac2.row, rac2.cell, 0x0A) == PageError::none);

    /* Clear it */
    page->clearGrapheme(rac.cell);
    page->updateRowGraphemeFlag(rac.row);
    ASSERT_TRUE(rac.row->grapheme());
    ASSERT_FALSE(rac.cell->hasGrapheme());
    ASSERT_TRUE(rac2.cell->hasGrapheme());
}

TEST(page, Page_clone) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp((uint32_t)y);

    /* Clone */
    Page page2;
    ASSERT_TRUE(page->clone(&page2));
    ASSERT_TRUE(page2.capacity == page->capacity);

    /* Read it again */
    for (size_t y = 0; y < page2.capacity.rows; y++) {
        ASSERT_TRUE(page2.getRowAndCell(1, y).cell->contentCodepoint() == y);
    }

    /* Write again */
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp(0);

    /* Read it again, should be unchanged */
    for (size_t y = 0; y < page2.capacity.rows; y++) {
        ASSERT_TRUE(page2.getRowAndCell(1, y).cell->contentCodepoint() == y);
    }

    /* Read the original */
    for (size_t y = 0; y < page->capacity.rows; y++) {
        ASSERT_TRUE(page->getRowAndCell(1, y).cell->contentCodepoint() == 0);
    }
    page2.deinit();
}

TEST(page, Page_clone_graphemes) {
    PageHolder page(cap(10, 10, 8));

    /* Append some graphemes */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        *rac.cell = Cell::init(0x09);
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0B) == PageError::none);
    }

    /* Clone it */
    Page page2;
    ASSERT_TRUE(page->clone(&page2));
    {
        const Page::RowAndCell rac = page2.getRowAndCell(0, 0);
        ASSERT_TRUE(rac.row->grapheme());
        ASSERT_TRUE(rac.cell->hasGrapheme());
        const uint32_t w[] = { 0x0A, 0x0B };
        ASSERT_TRUE(graphemeIs(page2, rac.cell, w, 2));
    }
    page2.deinit();
}

TEST(page, Page_clone_styles) {
    PageHolder page(cap(10, 10, 8));

    /* Write with some styles */
    {
        const style::Id id = addStyle(*page, boldStyle());

        for (size_t x = 0; x < page->size.cols; x++) {
            const Page::RowAndCell rac = page->getRowAndCell(x, 0);
            rac.row->setStyled(true);
            *rac.cell = cp((uint32_t)(x + 1));
            rac.cell->setStyleId(id);
            page->styles.use((const void *)page->memory, id);
        }
    }

    /* Clone it */
    Page page2;
    ASSERT_TRUE(page->clone(&page2));
    {
        const style::Id id = page2.getRowAndCell(0, 0).cell->style_id();

        for (size_t x = 0; x < page->size.cols; x++) {
            const Page::RowAndCell rac = page->getRowAndCell(x, 0);
            ASSERT_TRUE(rac.row->styled());
            ASSERT_TRUE(id == rac.cell->style_id());
        }

        const Style *st = page->styles.get((const void *)page->memory, id);
        ASSERT_TRUE(boldStyle().eql(*st));
    }
    page2.deinit();
}

TEST(page, Page_cloneFrom) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp((uint32_t)y);

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    ASSERT_TRUE(page2->cloneFrom(&*page, 0, page->size.rows) == PageError::none);

    /* Read it again */
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        ASSERT_TRUE(page2->getRowAndCell(1, y).cell->contentCodepoint() == y);
    }

    /* Write again */
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp(0);

    /* Read it again, should be unchanged */
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        ASSERT_TRUE(page2->getRowAndCell(1, y).cell->contentCodepoint() == y);
    }

    /* Read the original */
    for (size_t y = 0; y < page->capacity.rows; y++) {
        ASSERT_TRUE(page->getRowAndCell(1, y).cell->contentCodepoint() == 0);
    }
}

TEST(page, Page_cloneFrom_shrink_columns) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp((uint32_t)y);

    /* Clone */
    PageHolder page2(cap(5, 10, 8));
    ASSERT_TRUE(page2->cloneFrom(&*page, 0, page->size.rows) == PageError::none);
    ASSERT_TRUE(page2->size.cols == 5);

    /* Read it again */
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        ASSERT_TRUE(page2->getRowAndCell(1, y).cell->contentCodepoint() == y);
    }
}

TEST(page, Page_cloneFrom_partial) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp((uint32_t)y);

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    ASSERT_TRUE(page2->cloneFrom(&*page, 0, 5) == PageError::none);

    /* Read it again */
    for (size_t y = 0; y < 5; y++) ASSERT_TRUE(page2->getRowAndCell(1, y).cell->contentCodepoint() == y);
    for (size_t y = 5; y < page2->size.rows; y++) ASSERT_TRUE(page2->getRowAndCell(1, y).cell->contentCodepoint() == 0);
}

TEST(page, Page_cloneFrom_hyperlinks_exact_capacity) {
    PageHolder page(Capacity(50, 50));

    /* Ensure our page can accommodate the capacity. */
    const size_t hyperlink_cap = page->hyperlinkCapacity();
    ASSERT_TRUE(hyperlink_cap <= (size_t)page->size.cols * page->size.rows);

    /* Create a hyperlink. */
    hyperlink::Id hyperlink_id;
    ASSERT_TRUE(page->insertHyperlink(link("https://example.com", false, nullptr, 0), &hyperlink_id) == PageError::none);

    /* Fill the exact cap with cells. */
    bool done = false;
    for (size_t x = 0; x < page->size.cols && !done; x++) {
        for (size_t y = 0; y < page->size.rows; y++) {
            const Page::RowAndCell rac = page->getRowAndCell(x, y);
            *rac.cell = cp(42);
            ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, hyperlink_id) == PageError::none);
            page->hyperlink_set.use((const void *)page->memory, hyperlink_id);

            if (page->hyperlinkCount() == hyperlink_cap) {
                done = true;
                break;
            }
        }
    }
    ASSERT_TRUE(page->hyperlinkCount() == page->hyperlinkCapacity());

    /* Clone the full page */
    PageHolder page2(page->capacity);
    ASSERT_TRUE(page2->cloneFrom(&*page, 0, page->size.rows) == PageError::none);

    /* We should have the same number of hyperlinks */
    ASSERT_TRUE(page2->hyperlinkCount() == page->hyperlinkCount());
}

TEST(page, Page_cloneFrom_graphemes) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t y = 0; y < page->capacity.rows; y++) {
        const Page::RowAndCell rac = page->getRowAndCell(1, y);
        *rac.cell = cp((uint32_t)(y + 1));
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    ASSERT_TRUE(page2->cloneFrom(&*page, 0, page->size.rows) == PageError::none);

    const uint32_t w[] = { 0x0A };

    /* Read it again */
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        const Page::RowAndCell rac = page2->getRowAndCell(1, y);
        ASSERT_TRUE(rac.cell->contentCodepoint() == y + 1);
        ASSERT_TRUE(rac.row->grapheme());
        ASSERT_TRUE(rac.cell->hasGrapheme());
        ASSERT_TRUE(graphemeIs(*page2, rac.cell, w, 1));
    }

    /* Write again */
    for (size_t y = 0; y < page->capacity.rows; y++) {
        const Page::RowAndCell rac = page->getRowAndCell(1, y);
        page->clearGrapheme(rac.cell);
        page->updateRowGraphemeFlag(rac.row);
        *rac.cell = cp(0);
    }

    /* Read it again, should be unchanged */
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        const Page::RowAndCell rac = page2->getRowAndCell(1, y);
        ASSERT_TRUE(rac.cell->contentCodepoint() == y + 1);
        ASSERT_TRUE(rac.row->grapheme());
        ASSERT_TRUE(rac.cell->hasGrapheme());
        ASSERT_TRUE(graphemeIs(*page2, rac.cell, w, 1));
    }

    /* Read the original */
    for (size_t y = 0; y < page->capacity.rows; y++) {
        ASSERT_TRUE(page->getRowAndCell(1, y).cell->contentCodepoint() == 0);
    }
}

TEST(page, Page_cloneFrom_frees_dst_graphemes) {
    PageHolder page(cap(10, 10, 8));
    for (size_t y = 0; y < page->capacity.rows; y++) *page->getRowAndCell(1, y).cell = cp((uint32_t)(y + 1));

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        const Page::RowAndCell rac = page2->getRowAndCell(1, y);
        *rac.cell = cp((uint32_t)(y + 1));
        ASSERT_TRUE(page2->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }

    /* Clone from page which has no graphemes. */
    ASSERT_TRUE(page2->cloneFrom(&*page, 0, page->size.rows) == PageError::none);

    /* Read it again */
    for (size_t y = 0; y < page2->capacity.rows; y++) {
        const Page::RowAndCell rac = page2->getRowAndCell(1, y);
        ASSERT_TRUE(rac.cell->contentCodepoint() == y + 1);
        ASSERT_FALSE(rac.row->grapheme());
        ASSERT_FALSE(rac.cell->hasGrapheme());
    }
    ASSERT_TRUE(page2->graphemeCount() == 0);
}

TEST(page, Page_cloneRowFrom_partial) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) *page->getRowAndCell(x, 0).cell = cp((uint32_t)(x + 1));

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    ASSERT_TRUE(page2->clonePartialRowFrom(&*page, page2->getRow(0), page->getRow(0), 2, 8) == PageError::none);

    /* Read it again */
    for (size_t x = 0; x < page2->size.cols; x++) {
        const uint32_t expected = (x >= 2 && x < 8) ? (uint32_t)(x + 1) : 0;
        ASSERT_TRUE(page2->getRowAndCell(x, 0).cell->contentCodepoint() == expected);
    }
}

TEST(page, Page_cloneRowFrom_partial_grapheme_in_non_copied_source_region) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) *page->getRowAndCell(x, 0).cell = cp((uint32_t)(x + 1));
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }
    {
        const Page::RowAndCell rac = page->getRowAndCell(9, 0);
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }
    ASSERT_TRUE(page->graphemeCount() == 2);

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    ASSERT_TRUE(page2->clonePartialRowFrom(&*page, page2->getRow(0), page->getRow(0), 2, 8) == PageError::none);

    /* Read it again */
    for (size_t x = 0; x < page2->size.cols; x++) {
        const uint32_t expected = (x >= 2 && x < 8) ? (uint32_t)(x + 1) : 0;
        const Page::RowAndCell rac = page2->getRowAndCell(x, 0);
        ASSERT_TRUE(rac.cell->contentCodepoint() == expected);
        ASSERT_FALSE(rac.cell->hasGrapheme());
    }
    ASSERT_FALSE(page2->getRowAndCell(9, 0).row->grapheme());
    ASSERT_TRUE(page2->graphemeCount() == 0);
}

TEST(page, Page_cloneRowFrom_partial_grapheme_in_non_copied_dest_region) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) *page->getRowAndCell(x, 0).cell = cp((uint32_t)(x + 1));
    ASSERT_TRUE(page->graphemeCount() == 0);

    /* Clone */
    PageHolder page2(cap(10, 10, 8));
    for (size_t x = 0; x < page2->size.cols; x++) *page2->getRowAndCell(x, 0).cell = cp(0xBB);
    {
        const Page::RowAndCell rac = page2->getRowAndCell(0, 0);
        ASSERT_TRUE(page2->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }
    {
        const Page::RowAndCell rac = page2->getRowAndCell(9, 0);
        ASSERT_TRUE(page2->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }
    ASSERT_TRUE(page2->clonePartialRowFrom(&*page, page2->getRow(0), page->getRow(0), 2, 8) == PageError::none);

    /* Read it again */
    for (size_t x = 0; x < page2->size.cols; x++) {
        const uint32_t expected = (x >= 2 && x < 8) ? (uint32_t)(x + 1) : 0xBB;
        ASSERT_TRUE(page2->getRowAndCell(x, 0).cell->contentCodepoint() == expected);
    }
    ASSERT_TRUE(page2->getRowAndCell(9, 0).row->grapheme());
    ASSERT_TRUE(page2->graphemeCount() == 2);
}

static hyperlink::Id emptyImplicitLink(Page &page) {
    hyperlink::PageEntry e;
    e.id = hyperlink::PageEntry::Id::makeImplicit(0);
    hyperlink::Id id = 0;
    const ref_counted_set::AddError err =
        page.hyperlink_set.addContext((const void *)page.memory, e, hyperlink::SetContext(&page), &id);
    (void)err;
    return id;
}

TEST(page, Page_cloneRowFrom_partial_hyperlink_in_same_page_copy) {
    PageHolder page(Capacity(10, 10));

    /* We need to create a hyperlink. */
    const hyperlink::Id hyperlink_id = emptyImplicitLink(*page);

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) *page->getRowAndCell(x, 0).cell = cp((uint32_t)(x + 1));

    /* Hyperlink in a single cell */
    {
        const Page::RowAndCell rac = page->getRowAndCell(7, 0);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, hyperlink_id) == PageError::none);
    }
    ASSERT_TRUE(page->hyperlinkCount() == 1);

    /* Clone into the same page */
    ASSERT_TRUE(page->clonePartialRowFrom(&*page, page->getRow(1), page->getRow(0), 2, 8) == PageError::none);

    /* Read it again */
    for (size_t x = 0; x < page->size.cols; x++) {
        const uint32_t expected = (x >= 2 && x < 8) ? (uint32_t)(x + 1) : 0;
        ASSERT_TRUE(page->getRowAndCell(x, 1).cell->contentCodepoint() == expected);
    }
    {
        const Page::RowAndCell rac = page->getRowAndCell(7, 1);
        ASSERT_TRUE(rac.row->hyperlink());
        ASSERT_TRUE(rac.cell->hyperlink());
    }
    ASSERT_TRUE(page->hyperlinkCount() == 2);
}

TEST(page, Page_cloneRowFrom_partial_hyperlink_in_same_page_omit) {
    PageHolder page(Capacity(10, 10));

    /* We need to create a hyperlink. */
    const hyperlink::Id hyperlink_id = emptyImplicitLink(*page);

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) *page->getRowAndCell(x, 0).cell = cp((uint32_t)(x + 1));

    /* Hyperlink in a single cell */
    {
        const Page::RowAndCell rac = page->getRowAndCell(7, 0);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, hyperlink_id) == PageError::none);
    }
    ASSERT_TRUE(page->hyperlinkCount() == 1);

    /* Clone into the same page */
    ASSERT_TRUE(page->clonePartialRowFrom(&*page, page->getRow(1), page->getRow(0), 2, 6) == PageError::none);

    /* Read it again */
    for (size_t x = 0; x < page->size.cols; x++) {
        const uint32_t expected = (x >= 2 && x < 6) ? (uint32_t)(x + 1) : 0;
        ASSERT_TRUE(page->getRowAndCell(x, 1).cell->contentCodepoint() == expected);
    }
    {
        const Page::RowAndCell rac = page->getRowAndCell(7, 1);
        ASSERT_FALSE(rac.row->hyperlink());
        ASSERT_FALSE(rac.cell->hyperlink());
    }
    ASSERT_TRUE(page->hyperlinkCount() == 1);
}

TEST(page, Page_moveCells_text_only) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t x = 0; x < page->capacity.cols; x++) *page->getRowAndCell(x, 0).cell = cp((uint32_t)(x + 1));

    Row *src = page->getRow(0);
    Row *dst = page->getRow(1);
    page->moveCells(src, 0, dst, 0, page->capacity.cols);

    /* New rows should have text */
    for (size_t x = 0; x < page->capacity.cols; x++) {
        ASSERT_TRUE(page->getRowAndCell(x, 1).cell->contentCodepoint() == x + 1);
    }

    /* Old row should be blank */
    for (size_t x = 0; x < page->capacity.cols; x++) {
        ASSERT_TRUE(page->getRowAndCell(x, 0).cell->contentCodepoint() == 0);
    }
}

TEST(page, Page_moveCells_graphemes) {
    PageHolder page(cap(10, 10, 8));

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        *rac.cell = cp((uint32_t)(x + 1));
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }
    const size_t original_count = page->graphemeCount();

    Row *src = page->getRow(0);
    Row *dst = page->getRow(1);
    page->moveCells(src, 0, dst, 0, page->size.cols);
    ASSERT_TRUE(original_count == page->graphemeCount());

    const uint32_t w[] = { 0x0A };

    /* New rows should have text */
    for (size_t x = 0; x < page->size.cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 1);
        ASSERT_TRUE(rac.cell->contentCodepoint() == x + 1);
        ASSERT_TRUE(graphemeIs(*page, rac.cell, w, 1));
    }

    /* Old row should be blank */
    for (size_t x = 0; x < page->size.cols; x++) {
        ASSERT_TRUE(page->getRowAndCell(x, 0).cell->contentCodepoint() == 0);
    }
}

/* ─── verifyIntegrity ──────────────────────────────────────────────────── */

TEST(page, Page_verifyIntegrity_graphemes_good) {
    PageHolder page(cap(10, 10, 8));

    for (size_t x = 0; x < page->size.cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        *rac.cell = cp((uint32_t)(x + 1));
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }

    ASSERT_TRUE(page->verifyIntegrity() == PageError::none);
}

TEST(page, Page_verifyIntegrity_grapheme_row_not_marked) {
    PageHolder page(cap(10, 10, 8));

    for (size_t x = 0; x < page->size.cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        *rac.cell = cp((uint32_t)(x + 1));
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0A) == PageError::none);
    }

    /* Make invalid by unmarking the row */
    page->getRow(0)->setGrapheme(false);

    ASSERT_TRUE(page->verifyIntegrity() == PageError::UnmarkedGraphemeRow);
}

TEST(page, Page_verifyIntegrity_styles_good) {
    PageHolder page(cap(10, 10, 8));

    /* Upsert a style we'll use */
    const style::Id id = addStyle(*page, boldStyle());

    /* Write */
    for (size_t x = 0; x < page->size.cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        rac.row->setStyled(true);
        *rac.cell = cp((uint32_t)(x + 1));
        rac.cell->setStyleId(id);
        page->styles.use((const void *)page->memory, id);
    }

    /* The original style add would have incremented the
     * ref count too, so release it to balance that out. */
    page->styles.release((const void *)page->memory, id);

    ASSERT_TRUE(page->verifyIntegrity() == PageError::none);
}

TEST(page, Page_verifyIntegrity_styles_ref_count_mismatch) {
    PageHolder page(cap(10, 10, 8));

    const style::Id id = addStyle(*page, boldStyle());

    for (size_t x = 0; x < page->size.cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        rac.row->setStyled(true);
        *rac.cell = cp((uint32_t)(x + 1));
        rac.cell->setStyleId(id);
        page->styles.use((const void *)page->memory, id);
    }

    page->styles.release((const void *)page->memory, id);

    /* Miss a ref */
    page->styles.release((const void *)page->memory, id);

    ASSERT_TRUE(page->verifyIntegrity() == PageError::MismatchedStyleRef);
}

TEST(page, Page_verifyIntegrity_zero_rows) {
    PageHolder page(cap(10, 10, 8));
    page->size.rows = 0;
    ASSERT_TRUE(page->verifyIntegrity() == PageError::ZeroRowCount);
}

TEST(page, Page_verifyIntegrity_zero_cols) {
    PageHolder page(cap(10, 10, 8));
    page->size.cols = 0;
    ASSERT_TRUE(page->verifyIntegrity() == PageError::ZeroColCount);
}

/* ─── exactRowCapacity ─────────────────────────────────────────────────── */

static Capacity capWithLinks(size::CellCountInt cols, size::CellCountInt rows) {
    Capacity c = cap(cols, rows, 8);
    c.hyperlink_bytes = (size::HyperlinkCountInt)(32 * sizeof(hyperlink::Set::Item));
    c.string_bytes = 512;
    return c;
}

static void cloneRowsAndCompare(Page &page, size_t n) {
    const Capacity c = page.exactRowCapacity(0, n);
    PageHolder cloned(c);
    for (size_t y = 0; y < n; y++) {
        Row *src_row = &page.rows.ptr(page.memory)[y];
        Row *dst_row = &cloned->rows.ptr(cloned->memory)[y];
        ASSERT_TRUE(cloned->cloneRowFrom(&page, dst_row, src_row) == PageError::none);
    }
    const Capacity cloned_cap = cloned->exactRowCapacity(0, n);
    ASSERT_TRUE(c == cloned_cap);
}

TEST(page, Page_exactRowCapacity_empty_rows) {
    PageHolder page(capWithLinks(10, 10));

    /* Empty page: all capacity fields should be 0 (except cols/rows) */
    const Capacity c = page->exactRowCapacity(0, 5);
    ASSERT_TRUE(c.cols == 10);
    ASSERT_TRUE(c.rows == 5);
    ASSERT_TRUE(c.styles == 0);
    ASSERT_TRUE(c.grapheme_bytes == 0);
    ASSERT_TRUE(c.hyperlink_bytes == 0);
    ASSERT_TRUE(c.string_bytes == 0);
}

TEST(page, Page_exactRowCapacity_styles) {
    PageHolder page(cap(10, 10, 8));

    /* No styles: capacity should be 0 */
    ASSERT_TRUE(page->exactRowCapacity(0, 5).styles == 0);

    /* Add one style to a cell */
    const style::Id style1_id = addStyle(*page, boldStyle());
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        rac.row->setStyled(true);
        rac.cell->setStyleId(style1_id);
    }

    /* One unique style - capacity accounts for load factor */
    const Capacity cap_one_style = page->exactRowCapacity(0, 5);
    ASSERT_TRUE(StyleSet::capacityForCount(1) == cap_one_style.styles);

    /* Add same style to another cell (duplicate) - capacity unchanged */
    page->getRowAndCell(1, 0).cell->setStyleId(style1_id);
    ASSERT_TRUE(page->exactRowCapacity(0, 5).styles == cap_one_style.styles);

    /* Add a different style */
    Style italic;
    italic.flags.italic = true;
    const style::Id style2_id = addStyle(*page, italic);
    page->getRowAndCell(2, 0).cell->setStyleId(style2_id);

    /* Two unique styles - capacity accounts for load factor */
    const Capacity cap_two_styles = page->exactRowCapacity(0, 5);
    ASSERT_TRUE(StyleSet::capacityForCount(2) == cap_two_styles.styles);
    ASSERT_TRUE(cap_two_styles.styles > cap_one_style.styles);

    /* Style outside the row range should not be counted */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 7);
        rac.row->setStyled(true);
        Style ul;
        ul.flags.underline = ::wisp::terminal::sgr::Attribute::Underline::single;
        rac.cell->setStyleId(addStyle(*page, ul));
    }
    ASSERT_TRUE(page->exactRowCapacity(0, 5).styles == cap_two_styles.styles);

    /* Full range includes the new style */
    ASSERT_TRUE(page->exactRowCapacity(0, 10).styles == StyleSet::capacityForCount(3));

    /* Verify clone works with exact capacity and produces same result */
    cloneRowsAndCompare(*page, 5);
}

TEST(page, Page_exactRowCapacity_single_style_clone) {
    /* Regression test: verify a single style can be cloned with exact capacity.
     * This tests that capacityForCount properly accounts for ID 0 being reserved. */
    PageHolder page(cap(10, 2, 8));

    /* Add exactly one style to row 0 */
    const style::Id style_id = addStyle(*page, boldStyle());
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        rac.row->setStyled(true);
        rac.cell->setStyleId(style_id);
    }

    /* exactRowCapacity for just row 0 should give capacity for 1 style */
    const Capacity c = page->exactRowCapacity(0, 1);
    ASSERT_TRUE(StyleSet::capacityForCount(1) == c.styles);

    /* Create a new page with exact capacity and clone */
    PageHolder cloned(c);
    Row *src_row = &page->rows.ptr(page->memory)[0];
    Row *dst_row = &cloned->rows.ptr(cloned->memory)[0];

    /* This must not fail with StyleSetOutOfMemory */
    ASSERT_TRUE(cloned->cloneRowFrom(&*page, dst_row, src_row) == PageError::none);

    /* Verify the style was cloned correctly */
    const Cell *cloned_cell = &cloned->rows.ptr(cloned->memory)[0].cells().ptr(cloned->memory)[0];
    ASSERT_TRUE(cloned_cell->style_id() != style::default_id);
}

TEST(page, Page_exactRowCapacity_styles_max_single_row) {
    PageHolder page(cap(0xFFFF, 1, 0xFFFF));

    /* Style our first row */
    Row *row = &page->rows.ptr(page->memory)[0];
    row->setStyled(true);

    /* Fill cells with styles until we get OOM, but limit to a reasonable count
     * to avoid overflow when computing capacityForCount near maxInt */
    Cell *cells = row->cells().ptr(page->memory);
    size_t count = 0;
    const size_t max_count = 1000; /* Limit to avoid overflow in capacity calculation */
    for (size_t i = 0; i < page->size.cols; i++) {
        if (count >= max_count) break;
        Style s;
        s.fg_color = Style::Color::makeRgb(style::RGB((uint8_t)(i & 0xFF), (uint8_t)((i >> 8) & 0xFF), 0));
        style::Id style_id;
        if (page->styles.add((const void *)page->memory, s, &style_id) != ref_counted_set::AddError::none) break;
        cells[i].setStyleId(style_id);
        count += 1;
    }

    /* Verify we added a meaningful number of styles */
    ASSERT_TRUE(count > 0);

    /* Capacity should be at least count (adjusted for load factor) */
    ASSERT_TRUE(page->exactRowCapacity(0, 1).styles == StyleSet::capacityForCount(count));
}

TEST(page, Page_exactRowCapacity_grapheme_bytes) {
    PageHolder page(cap(10, 10, 8));

    /* No graphemes: capacity should be 0 */
    ASSERT_TRUE(page->exactRowCapacity(0, 5).grapheme_bytes == 0);

    /* Add one grapheme (1 codepoint) to a cell - rounds up to grapheme_chunk */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        *rac.cell = Cell::init('a');
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0301) == PageError::none); /* combining acute accent */
    }
    /* 1 codepoint = 4 bytes, rounds up to grapheme_chunk (16) */
    ASSERT_TRUE(page->exactRowCapacity(0, 5).grapheme_bytes == grapheme_chunk);

    /* Add another grapheme to a different cell - should sum */
    {
        const Page::RowAndCell rac = page->getRowAndCell(1, 0);
        *rac.cell = Cell::init('e');
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0300) == PageError::none); /* combining grave accent */
    }
    /* 2 graphemes, each 1 codepoint = 2 * grapheme_chunk */
    ASSERT_TRUE(page->exactRowCapacity(0, 5).grapheme_bytes == grapheme_chunk * 2);

    /* Add a larger grapheme (multiple codepoints) that fits in one chunk */
    {
        const Page::RowAndCell rac = page->getRowAndCell(2, 0);
        *rac.cell = Cell::init('o');
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0301) == PageError::none);
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0302) == PageError::none);
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0303) == PageError::none);
    }
    /* First two cells: 2 * grapheme_chunk
     * Third cell: 3 codepoints = 12 bytes, rounds up to grapheme_chunk */
    ASSERT_TRUE(page->exactRowCapacity(0, 5).grapheme_bytes == grapheme_chunk * 3);

    /* Grapheme outside the row range should not be counted */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 7);
        *rac.cell = Cell::init('x');
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, 0x0304) == PageError::none);
    }
    ASSERT_TRUE(page->exactRowCapacity(0, 5).grapheme_bytes == grapheme_chunk * 3);

    /* Full range includes the new grapheme */
    ASSERT_TRUE(page->exactRowCapacity(0, 10).grapheme_bytes == grapheme_chunk * 4);

    /* Verify clone works with exact capacity and produces same result */
    cloneRowsAndCompare(*page, 5);
}

TEST(page, Page_exactRowCapacity_grapheme_bytes_larger_than_chunk) {
    PageHolder page(cap(10, 10, 8));

    /* Add a grapheme larger than one chunk (grapheme_chunk_len = 4 codepoints) */
    const Page::RowAndCell rac = page->getRowAndCell(0, 0);
    *rac.cell = Cell::init('a');

    /* Add 6 codepoints - requires 2 chunks (6 * 4 = 24 bytes, rounds up to 32) */
    for (size_t i = 0; i < 6; i++) {
        ASSERT_TRUE(page->appendGrapheme(rac.row, rac.cell, (uint32_t)(0x0300 + i)) == PageError::none);
    }

    const Capacity c = page->exactRowCapacity(0, 1);
    /* 6 codepoints = 24 bytes, alignForward(24, 16) = 32 */
    ASSERT_TRUE(c.grapheme_bytes == 32);

    /* Verify clone works with exact capacity and produces same result */
    cloneRowsAndCompare(*page, 1);
}

TEST(page, Page_exactRowCapacity_hyperlinks) {
    PageHolder page(capWithLinks(10, 10));
    const size_t item = sizeof(hyperlink::Set::Item);

    /* No hyperlinks: capacity should be 0 */
    {
        const Capacity c = page->exactRowCapacity(0, 5);
        ASSERT_TRUE(c.hyperlink_bytes == 0);
        ASSERT_TRUE(c.string_bytes == 0);
    }

    /* Add one hyperlink with implicit ID */
    hyperlink::Id id1;
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);

        /* Create and add hyperlink entry */
        ASSERT_TRUE(page->insertHyperlink(link("https://example.com", false, nullptr, 1), &id1) == PageError::none);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id1) == PageError::none);
    }
    /* 1 hyperlink - capacity accounts for load factor */
    const Capacity cap_one_link = page->exactRowCapacity(0, 5);
    ASSERT_TRUE(hyperlink::Set::capacityForCount(1) * item == cap_one_link.hyperlink_bytes);
    /* URI "https://example.com" = 19 bytes, rounds up to string_chunk (32) */
    ASSERT_TRUE(string_chunk == cap_one_link.string_bytes);

    /* Add same hyperlink to another cell (duplicate ID) - capacity unchanged */
    {
        const Page::RowAndCell rac = page->getRowAndCell(1, 0);

        /* Use the same hyperlink ID for another cell */
        page->hyperlink_set.use((const void *)page->memory, id1);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id1) == PageError::none);
    }
    {
        const Capacity c = page->exactRowCapacity(0, 5);
        ASSERT_TRUE(cap_one_link.hyperlink_bytes == c.hyperlink_bytes);
        ASSERT_TRUE(cap_one_link.string_bytes == c.string_bytes);
    }

    /* Add a different hyperlink with explicit ID */
    {
        const Page::RowAndCell rac = page->getRowAndCell(2, 0);
        hyperlink::Id id;
        ASSERT_TRUE(page->insertHyperlink(link("https://other.example.org/path", true, "my-link-id", 0), &id) == PageError::none);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == PageError::none);
    }
    /* 2 hyperlinks - capacity accounts for load factor */
    const Capacity cap_two_links = page->exactRowCapacity(0, 5);
    ASSERT_TRUE(hyperlink::Set::capacityForCount(2) * item == cap_two_links.hyperlink_bytes);
    /* First URI: 19 bytes -> 32, Second URI: 30 bytes -> 32, Explicit ID: 10 bytes -> 32 */
    ASSERT_TRUE(string_chunk * 3 == cap_two_links.string_bytes);

    /* Hyperlink outside the row range should not be counted */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 7); /* row 7 is outside range [0, 5) */
        hyperlink::Id id;
        ASSERT_TRUE(page->insertHyperlink(link("https://outside.example.com", false, nullptr, 99), &id) == PageError::none);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == PageError::none);
    }
    {
        const Capacity c = page->exactRowCapacity(0, 5);
        ASSERT_TRUE(cap_two_links.hyperlink_bytes == c.hyperlink_bytes);
        ASSERT_TRUE(cap_two_links.string_bytes == c.string_bytes);
    }

    /* Full range includes the new hyperlink */
    {
        const Capacity c = page->exactRowCapacity(0, 10);
        ASSERT_TRUE(hyperlink::Set::capacityForCount(3) * item == c.hyperlink_bytes);
        /* Third URI: 27 bytes -> 32 */
        ASSERT_TRUE(string_chunk * 4 == c.string_bytes);
    }

    /* Verify clone works with exact capacity and produces same result */
    cloneRowsAndCompare(*page, 5);
}

TEST(page, Page_exactRowCapacity_single_hyperlink_clone) {
    /* Regression test: verify a single hyperlink can be cloned with exact capacity.
     * This tests that capacityForCount properly accounts for ID 0 being reserved. */
    PageHolder page(capWithLinks(10, 2));

    /* Add exactly one hyperlink to row 0 */
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        hyperlink::Id link_id;
        ASSERT_TRUE(page->insertHyperlink(link("https://example.com", false, nullptr, 1), &link_id) == PageError::none);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, link_id) == PageError::none);
    }

    /* exactRowCapacity for just row 0 should give capacity for 1 hyperlink */
    const Capacity c = page->exactRowCapacity(0, 1);
    ASSERT_TRUE(hyperlink::Set::capacityForCount(1) * sizeof(hyperlink::Set::Item) == c.hyperlink_bytes);

    /* Create a new page with exact capacity and clone */
    PageHolder cloned(c);
    Row *src_row = &page->rows.ptr(page->memory)[0];
    Row *dst_row = &cloned->rows.ptr(cloned->memory)[0];

    /* This must not fail with HyperlinkSetOutOfMemory */
    ASSERT_TRUE(cloned->cloneRowFrom(&*page, dst_row, src_row) == PageError::none);

    /* Verify the hyperlink was cloned correctly */
    ASSERT_TRUE(cloned->rows.ptr(cloned->memory)[0].cells().ptr(cloned->memory)[0].hyperlink());
}

TEST(page, Page_exactRowCapacity_hyperlink_map_capacity_for_many_cells) {
    /* A single hyperlink spanning many cells requires hyperlink_map capacity
     * based on cell count, not unique hyperlink count. */
    const size_t cols = 50;
    PageHolder page(capWithLinks((size::CellCountInt)cols, 2));

    /* Add one hyperlink spanning all 50 columns in row 0 */
    hyperlink::Id id;
    {
        const Page::RowAndCell rac = page->getRowAndCell(0, 0);
        ASSERT_TRUE(page->insertHyperlink(link("https://example.com", false, nullptr, 1), &id) == PageError::none);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == PageError::none);
    }

    /* Apply same hyperlink to remaining cells in row 0 */
    for (size_t x = 1; x < cols; x++) {
        const Page::RowAndCell rac = page->getRowAndCell(x, 0);
        page->hyperlink_set.use((const void *)page->memory, id);
        ASSERT_TRUE(page->setHyperlink(rac.row, rac.cell, id) == PageError::none);
    }

    /* exactRowCapacity must account for 50 hyperlink cells, not just 1 unique hyperlink */
    const Capacity c = page->exactRowCapacity(0, 1);

    /* The hyperlink_bytes must be large enough that layout() computes sufficient
     * hyperlink_map capacity. With hyperlink_cell_multiplier=16, we need at least
     * ceil(50/16) = 4 hyperlink entries worth of bytes for the map. */
    const size_t min_for_map = (cols + hyperlink_cell_multiplier - 1) / hyperlink_cell_multiplier;
    const size_t min_hyperlink_bytes = min_for_map * sizeof(hyperlink::Set::Item);
    ASSERT_TRUE(c.hyperlink_bytes >= min_hyperlink_bytes);

    /* Create a new page with exact capacity and clone - must not fail */
    PageHolder cloned(c);
    Row *src_row = &page->rows.ptr(page->memory)[0];
    Row *dst_row = &cloned->rows.ptr(cloned->memory)[0];

    /* This must not fail with HyperlinkMapOutOfMemory */
    ASSERT_TRUE(cloned->cloneRowFrom(&*page, dst_row, src_row) == PageError::none);

    /* Verify all hyperlinks were cloned correctly */
    for (size_t x = 0; x < cols; x++) {
        ASSERT_TRUE(cloned->rows.ptr(cloned->memory)[0].cells().ptr(cloned->memory)[x].hyperlink());
    }
}

TEST(page, Page_layout_avoids_double_rounding_hyperlink_map_capacity) {
    const size_t hyperlink_count = 3;
    Capacity c(1, 1);
    c.hyperlink_bytes = (size::HyperlinkCountInt)(hyperlink_count * sizeof(hyperlink::Set::Item));
    const Page::Layout layout = Page::layout(c);

    /* Three set entries request 48 usable map entries. Scaling that for the
     * 80% load factor needs 60 raw slots, which rounds once to 64. Rounding
     * the request before applying the load factor would allocate 128 slots. */
    ASSERT_TRUE(layout.hyperlink_map_layout.capacity == 64);
}

/* Wisp: upstream's type sizes this layout depends on. */
TEST(page, Wisp_item_sizes_match_upstream) {
    ASSERT_TRUE(sizeof(style::Style) == 28);
    ASSERT_TRUE(sizeof(StyleSet::Item) == 36);
    ASSERT_TRUE(sizeof(hyperlink::PageEntry) == 40);
    ASSERT_TRUE(sizeof(hyperlink::Set::Item) == 48);
}
