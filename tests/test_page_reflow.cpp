/* Tests for the resize and reflow operations in src/terminal/page.hpp.
 *
 * Related to Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Reflow is the operation a terminal's rows exist for: a line too long for
 * the screen is several rows joined by the wrap flag, and changing the width
 * means taking those runs apart and laying them out again. These tests write
 * text at one width, reflow it to another, and read the text back — the
 * question is always whether the line still says what it said, not how the
 * rows happened to be divided.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

struct PageBuf {
    uint64_t words[16384];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    PageBuf() { memset(words, 0, sizeof(words)); }
};

/* Write text across a page, soft-wrapping at the page's width exactly as a
 * terminal would. */
static void write_wrapped(Page *p, CellCountInt y, const char *text) {
    const CellCountInt cols = p->capacity.cols;
    CellCountInt x = 0;
    for (const char *c = text; *c; c++) {
        p->get_cell(x, y)->set_codepoint((uint32_t)(unsigned char)*c);
        x++;
        if (x == cols && c[1] != '\0') {
            p->get_row(y)->set_wrap(true);
            y++;
            p->get_row(y)->set_wrap_continuation(true);
            x = 0;
        }
    }
}

/* Read a logical line back out, following wrap flags and trimming the blanks
 * each row is padded with. */
static void read_line(Page *p, CellCountInt y, char *out, size_t out_len) {
    size_t n = 0;
    for (;;) {
        const CellCountInt width = page_row_used_width(p, y);
        for (CellCountInt x = 0; x < width && n + 1 < out_len; x++) {
            Cell *c = p->get_cell(x, y);
            if (c->wide() == Wide::spacer_head) continue;
            out[n++] = (char)c->codepoint();
        }
        if (!p->get_row(y)->wrap()) break;
        y++;
    }
    out[n] = '\0';
}

/* ─── narrowing ──────────────────────────────────────────────────────────── */

TEST(reflow, long_line_splits_when_narrowed) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(10, 8));

    write_wrapped(&src, 0, "abcdefghijklmnop");   /* 16 of 20 columns */

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rows_written, 2);

    /* Ten columns then six: the text is unchanged, only its shape is. */
    ASSERT_TRUE(dst.get_row(0)->wrap());
    ASSERT_TRUE(dst.get_row(1)->wrap_continuation());
    ASSERT_FALSE(dst.get_row(1)->wrap());

    char got[64];
    read_line(&dst, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdefghijklmnop") == 0);
}

TEST(reflow, already_wrapped_line_re_splits) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(10, 8));
    Page dst = Page::init(b.base(), Capacity(6, 8));

    /* Twenty-four characters: two source rows, four destination rows. */
    write_wrapped(&src, 0, "abcdefghijklmnopqrstuvwx");
    ASSERT_TRUE(src.get_row(0)->wrap());

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 3);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rows_written, 4);
    ASSERT_EQ(r.src_rows_consumed, 3);

    char got[64];
    read_line(&dst, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdefghijklmnopqrstuvwx") == 0);
}

/* ─── widening ───────────────────────────────────────────────────────────── */

TEST(reflow, wrapped_line_rejoins_when_widened) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(8, 8));
    Page dst = Page::init(b.base(), Capacity(40, 8));

    write_wrapped(&src, 0, "the quick brown fox");
    ASSERT_TRUE(src.get_row(0)->wrap());

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 3);
    ASSERT_TRUE(r.ok);

    /* Nineteen characters fit in forty columns, so the rows that only existed
     * because of the old width collapse back into one. */
    ASSERT_EQ(r.rows_written, 1);
    ASSERT_FALSE(dst.get_row(0)->wrap());

    char got[64];
    read_line(&dst, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "the quick brown fox") == 0);
}

/* ─── hard line ends ─────────────────────────────────────────────────────── */

TEST(reflow, unwrapped_rows_stay_separate) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(40, 8));

    /* Two lines the program ended itself. A wider screen must not join them,
     * which is the entire reason a soft wrap is distinguished from a hard
     * one. */
    write_wrapped(&src, 0, "first");
    write_wrapped(&src, 1, "second");

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 2);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rows_written, 2);

    char got[64];
    read_line(&dst, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "first") == 0);
    read_line(&dst, 1, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "second") == 0);
}

TEST(reflow, trailing_blanks_are_dropped) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    write_wrapped(&src, 0, "hi");

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(page_row_used_width(&dst, 0), 2);
}

TEST(reflow, spaces_are_content) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    /* A program that padded with spaces meant those spaces; trimming them
     * would silently change a line used for alignment. */
    write_wrapped(&src, 0, "a   ");

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(page_row_used_width(&dst, 0), 4);
}

/* ─── semantic prompts ───────────────────────────────────────────────────── */

TEST(reflow, prompt_mark_follows_the_line) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(8, 8));

    write_wrapped(&src, 0, "$ some long command");
    src.get_row(0)->set_semantic_prompt(SemanticPrompt::prompt);

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);

    /* The mark belongs to the line, so it lands on the line's first row and
     * is not smeared across the rows the new width produced. */
    ASSERT_TRUE(dst.get_row(0)->semantic_prompt() == SemanticPrompt::prompt);
    ASSERT_TRUE(dst.get_row(1)->semantic_prompt() == SemanticPrompt::none);
}

/* ─── wide characters ────────────────────────────────────────────────────── */

/* Place a double-width character and its spacer at (x, y). */
static void put_wide(Page *p, CellCountInt x, CellCountInt y, uint32_t cp) {
    Cell *lead = p->get_cell(x, y);
    lead->set_codepoint(cp);
    lead->set_wide(Wide::wide);

    Cell *tail = p->get_cell((CellCountInt)(x + 1), y);
    *tail = Cell();
    tail->set_wide(Wide::spacer_tail);
}

TEST(reflow, wide_pair_stays_together) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(5, 8));

    /* Four narrow cells then a wide one. At five columns the wide character
     * would straddle the edge, so it moves to the next row whole. */
    for (CellCountInt x = 0; x < 4; x++) {
        src.get_cell(x, 0)->set_codepoint('a' + x);
    }
    put_wide(&src, 4, 0, 0x4E2D);
    src.get_row(0)->set_wrap(true);
    src.get_row(1)->set_wrap_continuation(true);
    src.get_cell(0, 1)->set_codepoint('z');

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 2);
    ASSERT_TRUE(r.ok);

    /* The leftover column carries a spacer_head, which is what that state is
     * for: it says a wide character continues on the next row. */
    ASSERT_TRUE(dst.get_cell(4, 0)->wide() == Wide::spacer_head);
    ASSERT_TRUE(dst.get_row(0)->wrap());

    ASSERT_EQ(dst.get_cell(0, 1)->codepoint(), 0x4E2D);
    ASSERT_TRUE(dst.get_cell(0, 1)->wide() == Wide::wide);
    ASSERT_TRUE(dst.get_cell(1, 1)->wide() == Wide::spacer_tail);
    ASSERT_EQ(dst.get_cell(2, 1)->codepoint(), 'z');
}

TEST(reflow, old_spacer_head_is_discarded) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(5, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    /* A spacer_head records where the *old* width fell. Widening must drop
     * it, or the rejoined line carries a hole in the middle. */
    for (CellCountInt x = 0; x < 4; x++) src.get_cell(x, 0)->set_codepoint('a' + x);
    src.get_cell(4, 0)->set_wide(Wide::spacer_head);
    src.get_row(0)->set_wrap(true);
    src.get_row(1)->set_wrap_continuation(true);
    put_wide(&src, 0, 1, 0x4E2D);

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 2);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rows_written, 1);

    ASSERT_EQ(dst.get_cell(3, 0)->codepoint(), 'd');
    ASSERT_EQ(dst.get_cell(4, 0)->codepoint(), 0x4E2D);
    ASSERT_TRUE(dst.get_cell(4, 0)->wide() == Wide::wide);
    ASSERT_TRUE(dst.get_cell(5, 0)->wide() == Wide::spacer_tail);
}

/* ─── what the cells carry ───────────────────────────────────────────────── */

TEST(reflow, styles_move_with_their_cells) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(6, 8));

    style::Style s;
    s.fg_color.tag = style::StyleColor::Tag::palette;
    s.fg_color.palette = 42;

    write_wrapped(&src, 0, "abcdefghij");
    ASSERT_TRUE(src.set_cell_style(8, 0, s));

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);

    /* Column 8 of twenty becomes column 2 of the second six-wide row. The
     * style has to follow the character, not the position. */
    ASSERT_EQ(dst.get_cell(2, 1)->codepoint(), 'i');
    ASSERT_EQ(dst.get_cell_style(2, 1).fg_color.palette, 42);
}

TEST(reflow, hyperlinks_move_with_their_cells) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(6, 8));

    write_wrapped(&src, 0, "abcdefghij");
    ASSERT_TRUE(page_set_cell_hyperlink(&src, 7, 0, "https://moved.test", 18,
                                        nullptr, 0, 0));

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);

    const uint8_t *uri = nullptr;
    size_t len = 0;
    ASSERT_EQ(dst.get_cell(1, 1)->codepoint(), 'h');
    ASSERT_TRUE(page_get_cell_hyperlink(&dst, 1, 1, &uri, &len));
    ASSERT_EQ(len, 18u);
    ASSERT_TRUE(memcmp(uri, "https://moved.test", 18) == 0);
}

TEST(reflow, graphemes_move_with_their_cells) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(6, 8));

    write_wrapped(&src, 0, "abcdefghij");
    ASSERT_TRUE(page_append_grapheme(&src, 6, 0, 0x0301));

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_EQ(dst.get_cell(0, 1)->codepoint(), 'g');
    ASSERT_TRUE(page_grapheme_codepoints(&dst, 0, 1, &cps, &len));
    ASSERT_EQ(len, 1u);
    ASSERT_EQ(cps[0], 0x0301u);
}

/* ─── running out of room ────────────────────────────────────────────────── */

TEST(reflow, stops_when_destination_fills) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));

    Capacity shallow(20, 2);
    Page dst = Page::init(b.base(), shallow);

    for (CellCountInt y = 0; y < 5; y++) write_wrapped(&src, y, "line");

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 5);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.rows_written, 2);
    ASSERT_EQ(r.src_rows_consumed, 2);
}

TEST(reflow, partial_line_reports_its_start) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));

    Capacity shallow(6, 2);
    Page dst = Page::init(b.base(), shallow);

    /* One hard line, then a long one needing three six-wide rows. Only one
     * row is left for it, so it does not fit. */
    write_wrapped(&src, 0, "short");
    write_wrapped(&src, 1, "abcdefghijklmnop");

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 2);
    ASSERT_TRUE(r.ok);

    /* The source is reported consumed only up to the start of the line that
     * did not fit, so a caller continuing into another page picks the whole
     * line up again instead of splitting it across the seam. */
    ASSERT_EQ(r.src_rows_consumed, 1);
}

TEST(reflow, writing_into_a_used_destination_replaces_it) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    for (CellCountInt x = 0; x < 20; x++) dst.get_cell(x, 0)->set_codepoint('#');
    style::Style s;
    s.fg_color.tag = style::StyleColor::Tag::palette;
    s.fg_color.palette = 5;
    ASSERT_TRUE(dst.set_cell_style(0, 0, s));

    write_wrapped(&src, 0, "new");

    ReflowResult r = page_reflow_into(&dst, 0, &src, 0, 1);
    ASSERT_TRUE(r.ok);

    char got[64];
    read_line(&dst, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "new") == 0);
    /* The style the old contents held was released, not left interned. */
    ASSERT_EQ(dst.style_count(), 0u);
}

TEST(reflow, round_trip_through_a_narrow_page) {
    PageBuf a, b, c;
    Page wide1 = Page::init(a.base(), Capacity(24, 8));
    Page narrow = Page::init(b.base(), Capacity(7, 8));
    Page wide2 = Page::init(c.base(), Capacity(24, 8));

    /* Narrowing then widening again should give back the line it started as.
     * If reflow lost the difference between a soft wrap and a hard one, the
     * line would come back broken at the narrow width. */
    write_wrapped(&wide1, 0, "the quick brown fox jumps");

    ASSERT_TRUE(page_reflow_into(&narrow, 0, &wide1, 0, 2).ok);
    ASSERT_TRUE(page_reflow_into(&wide2, 0, &narrow, 0, 4).ok);

    char got[64];
    read_line(&wide2, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "the quick brown fox jumps") == 0);
}

TEST(reflow, source_is_not_modified) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(6, 8));

    write_wrapped(&src, 0, "abcdefghij");
    ASSERT_TRUE(page_set_cell_hyperlink(&src, 0, 0, "https://src.test", 16,
                                        nullptr, 0, 0));
    const size_t links = src.hyperlink_set.count();

    ASSERT_TRUE(page_reflow_into(&dst, 0, &src, 0, 1).ok);

    ASSERT_EQ(src.hyperlink_set.count(), links);
    char got[64];
    read_line(&src, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdefghij") == 0);
}
