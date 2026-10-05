/* Transliterated from the test blocks in Ghostty src/terminal/formatter.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: `t.vtStream()` is a stream_terminal Handler wrapped in a
 * stream::Stream, which is not copyable, so it is built in place by
 * StreamHolder as in the stream_terminal tests.
 */

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"
#include "../terminal/stream_terminal.hpp"
#include "../vt/formatter.hpp"
#include "../vt/terminal_formatter.hpp"

using namespace wisp;

namespace fmt = wisp::vt::formatter;
namespace st = wisp::terminal::stream_terminal;
namespace modes = wisp::terminal::modes;

typedef st::Handler Handler;
typedef wisp::terminal::stream::Stream<Handler> Stream;
typedef fmt::PageFormatter PageFormatter;
typedef fmt::Options Options;
typedef fmt::Format Format;
typedef fmt::Coordinate Coordinate;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

struct TermHolder {
    vt::Terminal t;
    bool ok;
    TermHolder(unsigned cols, unsigned rows) {
        vt::Terminal::Options o((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
        ok = vt::Terminal::init(talloc(), o, &t);
    }
    ~TermHolder() {
        if (ok) t.deinit(talloc());
    }
};

struct StreamHolder {
    Stream s;
    explicit StreamHolder(const Handler &h) : s(h, streamOptions()) {}
    ~StreamHolder() { s.deinit(); }

    static Stream::Options streamOptions() {
        Stream::Options o;
        o.allocator = true;
        return o;
    }
};

#define EXPECT_STR(expected, actual)                                                               \
    do {                                                                                           \
        const std::string _actual = (actual);                                                      \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                  \
    } while (0)

#define TERM(v, c, r)                                                                              \
    TermHolder v##_holder((c), (r));                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    vt::Terminal &v = v##_holder.t;                                                                \
    StreamHolder v##_stream(Handler::init(&v));                                                    \
    Stream &v##_s = v##_stream.s

/* Wisp: `t.screens.active.pages.pages.last.?.page()` with the
 * single-page assertions upstream makes first. */
static const fmt::Page *singlePage(vt::Terminal &t) {
    wisp::vt::PageList *pages = &t.screens.active->pages;
    if (pages->pages.first == nullptr) return nullptr;
    if (pages->pages.first != pages->pages.last) return nullptr;
    return pages->pages.last->page();
}

static bool coordEq(Coordinate a, vt::size::CellCountInt x, vt::size::CellCountInt y) {
    return a.x == x && a.y == y;
}

/* Wisp: `formatter.point_map = .{ .alloc = alloc, .map = &map }`. */
static void setPointMap(PageFormatter *f, std::vector<Coordinate> *map) {
    PageFormatter::PointMap pm;
    pm.map = map;
    pm.base = 0;
    f->point_map = vt::Maybe<PageFormatter::PointMap>(pm);
}

/* Wisp: `formatter.end_x = N` on a `?CellCountInt`. */
#define CCI(v) vt::Maybe<vt::size::CellCountInt>((vt::size::CellCountInt)(v))

/* Wisp: `for (a..b) |i| try testing.expectEqual(Coordinate{ .x = i + x0,
 * .y = y }, point_map.items[off + i])`. */
static bool pointRunIs(const std::vector<Coordinate> &m, size_t off, size_t n, unsigned x0,
                       unsigned y) {
    for (size_t i = 0; i < n; i++) {
        if (!coordEq(m[off + i], (vt::size::CellCountInt)(x0 + i), (vt::size::CellCountInt)y))
            return false;
    }
    return true;
}

/* Wisp: the shared preamble of the Page tests: a terminal, its stream,
 * the written input, and the single page they produce. */
#define PAGE_SETUP(t, c, r, input)                                                                     std::string builder;                                                                               TERM(t, (c), (r));                                                                                 t##_s.nextSlice(input);                                                                            const fmt::Page *page = singlePage(t);                                                              ASSERT_TRUE(page != nullptr)

TEST(formatter, Page_plain_single_line) {
    std::string builder;

    TERM(t, 80, 24);

    t_s.nextSlice("hello, world");

    /* Verify we have only a single page */
    const fmt::Page *page = singlePage(t);
    ASSERT_TRUE(page != nullptr);

    /* Create the formatter */
    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    /* Test our point map. */
    std::vector<Coordinate> point_map;
    {
        PageFormatter::PointMap pm;
        pm.map = &point_map;
        pm.base = 0;
        formatter.point_map = vt::Maybe<PageFormatter::PointMap>(pm);
    }

    /* Verify output */
    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello, world", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 12) == state.cells);

    /* Verify our point map */
    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < builder.size(); i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
}

TEST(formatter, Page_plain_single_line_soft_wrapped_unwrapped) {
    std::string builder;

    TERM(t, 3, 5);

    t_s.nextSlice("hello!");

    const fmt::Page *page = singlePage(t);
    ASSERT_TRUE(page != nullptr);

    Options opts(Format::plain, true);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    {
        PageFormatter::PointMap pm;
        pm.map = &point_map;
        pm.base = 0;
        formatter.point_map = vt::Maybe<PageFormatter::PointMap>(pm);
    }

    /* Verify output
     * Note: we don't test the trailing state, which may have bugs
     * with unwrap... */
    (void)formatter.formatWithState(&builder);
    EXPECT_STR("hello!", builder);

    /* Verify our point map */
    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(coordEq(point_map[0], 0, 0));
    ASSERT_TRUE(coordEq(point_map[1], 1, 0));
    ASSERT_TRUE(coordEq(point_map[2], 2, 0));
    ASSERT_TRUE(coordEq(point_map[3], 0, 1));
    ASSERT_TRUE(coordEq(point_map[4], 1, 1));
    ASSERT_TRUE(coordEq(point_map[5], 2, 1));
}


TEST(formatter, Page_plain_single_wide_char) {
    PAGE_SETUP(t, 80, 24, "1A\xE2\x9A\xA1");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    /* Full string */
    {
        builder.clear();
        point_map.clear();
        const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
        EXPECT_STR("1A\xE2\x9A\xA1", builder);
        ASSERT_TRUE((size_t)page->size.rows == state.rows);
        ASSERT_TRUE((size_t)(page->size.cols - 4) == state.cells);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 2; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 2, 0));
    }

    /* Wide only (from start) */
    {
        builder.clear();
        point_map.clear();

        formatter.start_x = 2;
        const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
        EXPECT_STR("\xE2\x9A\xA1", builder);
        ASSERT_TRUE((size_t)page->size.rows == state.rows);
        ASSERT_TRUE((size_t)(page->size.cols - 4) == state.cells);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 0; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 2, 0));
    }

    /* Wide only (from tail) */
    {
        builder.clear();
        point_map.clear();

        formatter.start_x = 3;
        const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
        EXPECT_STR("\xE2\x9A\xA1", builder);
        ASSERT_TRUE((size_t)page->size.rows == state.rows);
        ASSERT_TRUE((size_t)(page->size.cols - 4) == state.cells);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 0; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 2, 0));
    }
}

TEST(formatter, Page_plain_single_wide_char_soft_wrapped_unwrapped) {
    PAGE_SETUP(t, 3, 24, "1A\xE2\x9A\xA1");

    Options opts(Format::plain);
    opts.unwrap = true;
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    /* Full string */
    {
        builder.clear();
        point_map.clear();
        const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
        EXPECT_STR("1A\xE2\x9A\xA1", builder);
        ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
        ASSERT_TRUE((size_t)(page->size.cols - 2) == state.cells);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 2; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 0, 1));
    }

    /* Full string (ending on spacer head) */
    {
        builder.clear();
        point_map.clear();

        formatter.end_x = vt::Maybe<vt::size::CellCountInt>((vt::size::CellCountInt)2);
        formatter.end_y = vt::Maybe<vt::size::CellCountInt>((vt::size::CellCountInt)0);

        (void)formatter.formatWithState(&builder);
        EXPECT_STR("1A\xE2\x9A\xA1", builder);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 2; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 0, 1));

        formatter.end_x = vt::Maybe<vt::size::CellCountInt>();
        formatter.end_y = vt::Maybe<vt::size::CellCountInt>();
    }

    /* Wide only (from start) */
    {
        builder.clear();
        point_map.clear();

        formatter.start_x = 2;
        const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
        EXPECT_STR("\xE2\x9A\xA1", builder);
        ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
        ASSERT_TRUE((size_t)(page->size.cols - 2) == state.cells);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 0; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 0, 1));
    }

    /* Wide only (from tail) */
    {
        builder.clear();
        point_map.clear();

        formatter.start_y = 1;
        formatter.start_x = 1;
        const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
        EXPECT_STR("\xE2\x9A\xA1", builder);
        ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
        ASSERT_TRUE((size_t)(page->size.cols - 2) == state.cells);

        ASSERT_TRUE(builder.size() == point_map.size());
        for (size_t i = 0; i < builder.size(); i++) ASSERT_TRUE(coordEq(point_map[i], 0, 1));
    }
}

TEST(formatter, Page_plain_multiline) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello\nworld", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[5], 4, 0)); /* \n */
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[6 + i], (vt::size::CellCountInt)i, 1));
}

TEST(formatter, Page_plain_multiline_rectangle) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_x = 1;
    formatter.end_x = vt::Maybe<vt::size::CellCountInt>((vt::size::CellCountInt)3);
    formatter.rectangle = true;

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("ell\norl", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE(0 == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 3; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)(i + 1), 0));
    ASSERT_TRUE(coordEq(point_map[3], 3, 0)); /* \n */
    for (size_t i = 0; i < 3; i++)
        ASSERT_TRUE(coordEq(point_map[4 + i], (vt::size::CellCountInt)(i + 1), 1));
}

TEST(formatter, Page_plain_multi_blank_lines) {
    PAGE_SETUP(t, 80, 24, "hello\r\n\r\n\r\nworld");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello\n\n\nworld", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 3) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[5], 4, 0)); /* \n after row 0 */
    ASSERT_TRUE(coordEq(point_map[6], 0, 1)); /* \n after blank row 1 */
    ASSERT_TRUE(coordEq(point_map[7], 0, 2)); /* \n after blank row 2 */
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[8 + i], (vt::size::CellCountInt)i, 3));
}

TEST(formatter, Page_plain_trailing_blank_lines) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld\r\n\r\n");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    /* Verify output. We expect there to be no trailing newlines because
     * we can't differentiate trailing blank lines as being meaningful because
     * the page formatter can't see the cursor position. */
    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello\nworld", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[5], 4, 0)); /* \n */
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[6 + i], (vt::size::CellCountInt)i, 1));
}

TEST(formatter, Page_plain_trailing_whitespace) {
    PAGE_SETUP(t, 80, 24, "hello   \r\nworld   ");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello\nworld", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[5], 4, 0)); /* \n */
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[6 + i], (vt::size::CellCountInt)i, 1));
}

TEST(formatter, Page_plain_trailing_whitespace_no_trim) {
    PAGE_SETUP(t, 80, 24, "hello   \r\nworld  ");

    Options opts(Format::plain, false, false);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello   \nworld  ", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 7) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 8; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[8], 7, 0)); /* \n */
    for (size_t i = 0; i < 7; i++)
        ASSERT_TRUE(coordEq(point_map[9 + i], (vt::size::CellCountInt)i, 1));
}


/* Wisp: `formatter.trailing_state = .{ .rows = r, .cells = c }`. */
static void setTrailingState(PageFormatter *f, size_t rows, size_t cells) {
    PageFormatter::TrailingState ts;
    ts.rows = rows;
    ts.cells = cells;
    f->trailing_state = vt::Maybe<PageFormatter::TrailingState>(ts);
}

TEST(formatter, Page_plain_with_prior_trailing_state_rows) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    setTrailingState(&formatter, 2, 0);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("\n\nhello", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(coordEq(point_map[0], 0, 0)); /* \n first blank row */
    ASSERT_TRUE(coordEq(point_map[1], 0, 1)); /* \n second blank row */
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[2 + i], (vt::size::CellCountInt)i, 0));
}

TEST(formatter, Page_plain_with_prior_trailing_state_cells_no_wrapped_line) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    setTrailingState(&formatter, 0, 3);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Blank cells are reset when row is not a wrap continuation */
    EXPECT_STR("hello", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
}

TEST(formatter, Page_plain_with_prior_trailing_state_cells_with_wrap_continuation) {
    PAGE_SETUP(t, 80, 24, "world");

    /* Surgically modify the first row to be a wrap continuation */
    page->getRow(0)->setWrapContinuation(true);

    Options opts(Format::plain, true);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    setTrailingState(&formatter, 0, 3);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Blank cells are preserved when row is a wrap continuation with unwrap enabled */
    EXPECT_STR("   world", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    /* Verify point map - 3 spaces from prior trailing state + "world" */
    ASSERT_TRUE(builder.size() == point_map.size());
    /* The 3 blank cells can't go back beyond (0,0) so they all map to (0,0) */
    ASSERT_TRUE(coordEq(point_map[0], 0, 0)); /* space */
    ASSERT_TRUE(coordEq(point_map[1], 0, 0)); /* space */
    ASSERT_TRUE(coordEq(point_map[2], 0, 0)); /* space */
    for (size_t i = 0; i < 5; i++)
        ASSERT_TRUE(coordEq(point_map[3 + i], (vt::size::CellCountInt)i, 0));
}

TEST(formatter, Page_plain_soft_wrapped_without_unwrap) {
    PAGE_SETUP(t, 10, 24, "hello world test");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Without unwrap, wrapped lines show as separate lines */
    EXPECT_STR("hello worl\nd test", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 6) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[10], 9, 0)); /* \n */
    for (size_t i = 0; i < 6; i++)
        ASSERT_TRUE(coordEq(point_map[11 + i], (vt::size::CellCountInt)i, 1));
}

TEST(formatter, Page_plain_soft_wrapped_with_unwrap) {
    PAGE_SETUP(t, 10, 24, "hello world test");

    Options opts(Format::plain, true);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* With unwrap, wrapped lines are joined together */
    EXPECT_STR("hello world test", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 6) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    for (size_t i = 0; i < 6; i++)
        ASSERT_TRUE(coordEq(point_map[10 + i], (vt::size::CellCountInt)i, 1));
}

TEST(formatter, Page_plain_soft_wrapped_3_lines_without_unwrap) {
    PAGE_SETUP(t, 10, 24, "hello world this is a test");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Without unwrap, wrapped lines show as separate lines */
    EXPECT_STR("hello worl\nd this is\na test", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 2) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 6) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    ASSERT_TRUE(coordEq(point_map[10], 9, 0)); /* \n */
    for (size_t i = 0; i < 9; i++)
        ASSERT_TRUE(coordEq(point_map[11 + i], (vt::size::CellCountInt)i, 1));
    ASSERT_TRUE(coordEq(point_map[20], 8, 1)); /* \n */
    for (size_t i = 0; i < 6; i++)
        ASSERT_TRUE(coordEq(point_map[21 + i], (vt::size::CellCountInt)i, 2));
}

TEST(formatter, Page_plain_soft_wrapped_3_lines_with_unwrap) {
    PAGE_SETUP(t, 10, 24, "hello world this is a test");

    Options opts(Format::plain, true);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* With unwrap, wrapped lines are joined together */
    EXPECT_STR("hello world this is a test", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 2) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 6) == state.cells);

    /* Verify point map - unwrapped text spans 3 rows */
    ASSERT_TRUE(builder.size() == point_map.size());
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(coordEq(point_map[i], (vt::size::CellCountInt)i, 0));
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(coordEq(point_map[10 + i], (vt::size::CellCountInt)i, 1));
    for (size_t i = 0; i < 6; i++)
        ASSERT_TRUE(coordEq(point_map[20 + i], (vt::size::CellCountInt)i, 2));
}


TEST(formatter, Page_plain_start_y_subset) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld\r\ntest");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_y = 1;

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("world\ntest", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 2) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 4) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 1));
    ASSERT_TRUE(coordEq(point_map[5], 4, 1)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 6, 4, 0, 2));
}

TEST(formatter, Page_plain_end_y_subset) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld\r\ntest");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.end_y = CCI(1);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello\nworld", builder);
    ASSERT_TRUE(1 == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 0));
    ASSERT_TRUE(coordEq(point_map[5], 4, 0)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 6, 5, 0, 1));
}

TEST(formatter, Page_plain_start_y_and_end_y_range) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld\r\ntest\r\nfoo");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_y = 1;
    formatter.end_y = CCI(2);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("world\ntest", builder);
    ASSERT_TRUE(1 == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 4) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 1));
    ASSERT_TRUE(coordEq(point_map[5], 4, 1)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 6, 4, 0, 2));
}

TEST(formatter, Page_plain_start_y_out_of_bounds) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_y = 30;

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("", builder);
    ASSERT_TRUE(0 == state.rows);
    ASSERT_TRUE(0 == state.cells);

    /* Verify point map is empty */
    ASSERT_TRUE(0 == point_map.size());
}

TEST(formatter, Page_plain_end_y_greater_than_rows) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.end_y = CCI(30);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Should clamp to page.size.rows and work normally */
    EXPECT_STR("hello", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 0));
}

TEST(formatter, Page_plain_end_y_less_than_start_y) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_y = 5;
    formatter.end_y = CCI(2);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("", builder);
    ASSERT_TRUE(0 == state.rows);
    ASSERT_TRUE(0 == state.cells);

    /* Verify point map is empty */
    ASSERT_TRUE(0 == point_map.size());
}

TEST(formatter, Page_plain_start_x_on_first_row_only) {
    PAGE_SETUP(t, 80, 24, "hello world");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_x = 6;

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("world", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 11) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 6, 0));
}

TEST(formatter, Page_plain_end_x_on_last_row_only) {
    PAGE_SETUP(t, 80, 24, "first line\r\nsecond line\r\nthird line");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.end_y = CCI(2);
    formatter.end_x = CCI(4);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("first line\nsecond line\nthird", builder);
    ASSERT_TRUE(1 == state.rows);
    ASSERT_TRUE(0 == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 10, 0, 0));
    ASSERT_TRUE(coordEq(point_map[10], 9, 0)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 11, 11, 0, 1));
    ASSERT_TRUE(coordEq(point_map[22], 10, 1)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 23, 5, 0, 2));
}


TEST(formatter, Page_plain_start_x_and_end_x_multiline) {
    PAGE_SETUP(t, 80, 24, "hello world\r\ntest case\r\nfoo bar");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_x = 6;
    formatter.end_y = CCI(2);
    formatter.end_x = CCI(2);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* First row: "world" (start_x=6 to end of row)
     * Second row: "test case" (full row)
     * Third row: "foo" (start to end_x=2, inclusive) */
    EXPECT_STR("world\ntest case\nfoo", builder);
    ASSERT_TRUE(1 == state.rows);
    ASSERT_TRUE(0 == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 6, 0));
    ASSERT_TRUE(coordEq(point_map[5], 10, 0)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 6, 9, 0, 1));
    ASSERT_TRUE(coordEq(point_map[15], 8, 1)); /* \n */
    ASSERT_TRUE(pointRunIs(point_map, 16, 3, 0, 2));
}

TEST(formatter, Page_plain_start_x_out_of_bounds) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_x = 100;

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("", builder);
    ASSERT_TRUE(0 == state.rows);
    ASSERT_TRUE(0 == state.cells);

    /* Verify point map is empty */
    ASSERT_TRUE(0 == point_map.size());
}

TEST(formatter, Page_plain_end_x_greater_than_cols) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.end_x = CCI(100);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 0));
}

TEST(formatter, Page_plain_end_x_less_than_start_x_single_row) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_x = 10;
    formatter.end_y = CCI(0);
    formatter.end_x = CCI(5);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("", builder);
    ASSERT_TRUE(0 == state.rows);
    ASSERT_TRUE(0 == state.cells);

    /* Verify point map is empty */
    ASSERT_TRUE(0 == point_map.size());
}

TEST(formatter, Page_plain_start_y_non_zero_ignores_trailing_state) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_y = 1;
    setTrailingState(&formatter, 5, 10);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Should NOT output the 5 newlines from trailing_state because start_y is non-zero */
    EXPECT_STR("world", builder);
    ASSERT_TRUE((size_t)(page->size.rows - 1) == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 1));
}

TEST(formatter, Page_plain_start_x_non_zero_ignores_trailing_state) {
    PAGE_SETUP(t, 80, 24, "hello world");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_x = 6;
    setTrailingState(&formatter, 2, 8);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* Should NOT output the 2 newlines or 8 spaces from trailing_state because
     * start_x is non-zero */
    EXPECT_STR("world", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 11) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 6, 0));
}

TEST(formatter, Page_plain_start_y_and_start_x_zero_uses_trailing_state) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.start_y = 0;
    formatter.start_x = 0;
    setTrailingState(&formatter, 2, 0);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* SHOULD output the 2 newlines from trailing_state because both start_y and
     * start_x are 0 */
    EXPECT_STR("\n\nhello", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 5) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(coordEq(point_map[0], 0, 0)); /* \n first blank row */
    ASSERT_TRUE(coordEq(point_map[1], 0, 1)); /* \n second blank row */
    ASSERT_TRUE(pointRunIs(point_map, 2, 5, 0, 0));
}

TEST(formatter, Page_plain_single_line_with_styling) {
    PAGE_SETUP(t, 80, 24, "hello, \x1b[1mworld\x1b[0m");

    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    EXPECT_STR("hello, world", builder);
    ASSERT_TRUE((size_t)page->size.rows == state.rows);
    ASSERT_TRUE((size_t)(page->size.cols - 12) == state.cells);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 12, 0, 0));
}


/* Wisp: `.{ .r = r, .g = g, .b = b }` as a Maybe<RGB>. */
static vt::Maybe<terminal::RGB> rgb(uint8_t r, uint8_t g, uint8_t b) {
    terminal::RGB c;
    c.r = r;
    c.g = g;
    c.b = b;
    return vt::Maybe<terminal::RGB>(c);
}

TEST(formatter, Page_VT_single_line_plain_text) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("hello", builder);

    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 5, 0, 0));
}

TEST(formatter, Page_VT_single_line_with_bold) {
    PAGE_SETUP(t, 80, 24, "\x1b[1mhello\x1b[0m");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("\x1b[0m\x1b[1mhello\x1b[0m", builder);

    /* Verify point map - style sequences should point to first character they style */
    ASSERT_TRUE(builder.size() == point_map.size());
    /* \x1b[0m = 4 bytes, \x1b[1m = 4 bytes, total 8 bytes of style sequences
     * All style bytes should map to the first styled character at (0, 0) */
    for (size_t i = 0; i < 8; i++) ASSERT_TRUE(coordEq(point_map[i], 0, 0));
    /* Then "hello" maps to its respective positions */
    ASSERT_TRUE(pointRunIs(point_map, 8, 5, 0, 0));
}

TEST(formatter, Page_VT_multiple_styles) {
    PAGE_SETUP(t, 80, 24, "\x1b[1mhello \x1b[3mworld\x1b[0m");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("\x1b[0m\x1b[1mhello \x1b[0m\x1b[1m\x1b[3mworld\x1b[0m", builder);

    /* Verify point map matches output length */
    ASSERT_TRUE(builder.size() == point_map.size());
}

TEST(formatter, Page_VT_with_foreground_color) {
    PAGE_SETUP(t, 80, 24, "\x1b[31mred\x1b[0m");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("\x1b[0m\x1b[38;5;1mred\x1b[0m", builder);

    /* Verify point map - style sequences should point to first character they style */
    ASSERT_TRUE(builder.size() == point_map.size());
    /* \x1b[0m = 4 bytes, \x1b[38;5;1m = 9 bytes, total 13 bytes of style sequences
     * All style bytes should map to the first styled character at (0, 0) */
    for (size_t i = 0; i < 13; i++) ASSERT_TRUE(coordEq(point_map[i], 0, 0));
    /* Then "red" maps to its respective positions */
    ASSERT_TRUE(pointRunIs(point_map, 13, 3, 0, 0));
}

TEST(formatter, Page_VT_with_background_and_foreground_colors) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::vt);
    opts.background = rgb(0x12, 0x34, 0x56);
    opts.foreground = rgb(0xab, 0xcd, 0xef);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    formatter.format(&builder);
    /* Should emit OSC 10 for foreground, OSC 11 for background, then the text */
    EXPECT_STR("\x1b]10;rgb:ab/cd/ef\x1b\\\x1b]11;rgb:12/34/56\x1b\\hello", builder);
}

TEST(formatter, Page_VT_multi_line_with_styles) {
    PAGE_SETUP(t, 80, 24, "\x1b[1mfirst\x1b[0m\r\n\x1b[3msecond\x1b[0m");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    /* Note: style is reset before newline to prevent background colors from
     * bleeding to the next line's leading cells. */
    EXPECT_STR("\x1b[0m\x1b[1mfirst\x1b[0m\r\n\x1b[0m\x1b[3msecond\x1b[0m", builder);

    /* Verify point map matches output length */
    ASSERT_TRUE(builder.size() == point_map.size());
}

TEST(formatter, Page_VT_duplicate_style_not_emitted_twice) {
    PAGE_SETUP(t, 80, 24, "\x1b[1mhel\x1b[1mlo\x1b[0m");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("\x1b[0m\x1b[1mhello\x1b[0m", builder);

    /* Verify point map matches output length */
    ASSERT_TRUE(builder.size() == point_map.size());
}


/* ─── PageList ─────────────────────────────────────────────────────────── */

typedef fmt::PageListFormatter PageListFormatter;
typedef fmt::PinMap PinMap;
typedef fmt::Pin Pin;

/* Wisp: `formatter.pin_map = .{ .alloc = alloc, .map = &map }`. */
static void setPinMap(PageListFormatter *f, PinMap::Map *map) {
    PinMap pm;
    pm.map = map;
    f->pin_map = vt::Maybe<PinMap>(pm);
}

/* Wisp: `std.mem.trimStart(u8, s, chars)`. */
static std::string trimStart(const std::string &s, const char *chars) {
    size_t i = 0;
    while (i < s.size() && strchr(chars, s[i]) != nullptr) i += 1;
    return s.substr(i);
}

/* Wisp: `.{ .node = n, .y = y, .x = x }` */
static Pin pinAt(PinMap::Node node, unsigned y, unsigned x) {
    return Pin(node, (vt::size::CellCountInt)y, (vt::size::CellCountInt)x);
}

/* Wisp: the shared preamble of the PageList tests. */
#define LIST_SETUP(t, c, r, input)                                                                 \
    std::string builder;                                                                           \
    TERM(t, (c), (r));                                                                             \
    t##_s.nextSlice(input);                                                                        \
    wisp::vt::PageList *pages = &t.screens.active->pages

TEST(formatter, PageList_plain_single_line) {
    LIST_SETUP(t, 80, 24, "hello, world");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("hello, world", builder);

    /* Verify pin map */
    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = pages->pages.first;
    for (size_t i = 0; i < builder.size(); i++) {
        const vt::Maybe<Pin> got = pin_map.get(i);
        ASSERT_TRUE(got.has);
        ASSERT_TRUE(got.value.node == node);
        ASSERT_TRUE(got.value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(got.value.y == 0);
    }
    (void)pinAt(node, 0, 0);
}

TEST(formatter, PageList_plain_spanning_two_pages) {
    std::string builder;
    TERM(t, 80, 24);
    wisp::vt::PageList *pages = &t.screens.active->pages;

    const size_t first_page_rows = pages->pages.first->capacity().rows;

    /* Fill the first page almost completely */
    for (size_t i = 0; i < first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("page one");

    /* Verify we're still on one page */
    ASSERT_TRUE(pages->pages.first == pages->pages.last);

    /* Add one more newline to push content to a second page */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(pages->pages.first != pages->pages.last);

    /* Write content on the second page */
    t_s.nextSlice("page two");

    /* Format the entire PageList */
    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    const std::string output = trimStart(builder, "\n");
    ASSERT_STR_EQ("page one\npage two", output.c_str());

    /* Verify pin map */
    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node first_node = pages->pages.first;
    PinMap::Node last_node = pages->pages.last;
    const size_t trimmed_count = builder.size() - output.size();

    /* First part (trimmed blank lines) maps to first node */
    for (size_t i = 0; i < trimmed_count; i++) ASSERT_TRUE(pin_map.get(i).value.node == first_node);

    /* "page one" (8 chars) maps to first node */
    for (size_t i = 0; i < 8; i++) {
        const size_t idx = trimmed_count + i;
        ASSERT_TRUE(pin_map.get(idx).value.node == first_node);
        ASSERT_TRUE(pin_map.get(idx).value.x == (vt::size::CellCountInt)i);
    }

    /* \n - maps to last node as it represents the transition to new page */
    ASSERT_TRUE(pin_map.get(trimmed_count + 8).value.node == last_node);

    /* "page two" (8 chars) maps to last node */
    for (size_t i = 0; i < 8; i++) {
        const size_t idx = trimmed_count + 9 + i;
        ASSERT_TRUE(pin_map.get(idx).value.node == last_node);
        ASSERT_TRUE(pin_map.get(idx).value.x == (vt::size::CellCountInt)i);
    }
}

TEST(formatter, PageList_soft_wrapped_line_spanning_two_pages_without_unwrap) {
    std::string builder;
    TERM(t, 10, 3);
    wisp::vt::PageList *pages = &t.screens.active->pages;

    const size_t first_page_rows = pages->pages.first->capacity().rows;

    /* Fill the first page with soft-wrapped content */
    for (size_t i = 0; i < first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("hello world test");

    /* Verify we're on two pages due to wrapping */
    ASSERT_TRUE(pages->pages.first != pages->pages.last);

    /* Format without unwrap - should show line breaks */
    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    const std::string output = trimStart(builder, "\n");
    ASSERT_STR_EQ("hello worl\nd test", output.c_str());

    /* Verify pin map */
    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node first_node = pages->pages.first;
    PinMap::Node last_node = pages->pages.last;
    const size_t trimmed_count = builder.size() - output.size();

    for (size_t i = 0; i < trimmed_count; i++) ASSERT_TRUE(pin_map.get(i).value.node == first_node);
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(pin_map.get(trimmed_count + i).value.node == first_node);
    ASSERT_TRUE(pin_map.get(trimmed_count + 10).value.node == last_node);
    for (size_t i = 0; i < 6; i++)
        ASSERT_TRUE(pin_map.get(trimmed_count + 11 + i).value.node == last_node);
}

TEST(formatter, PageList_soft_wrapped_line_spanning_two_pages_with_unwrap) {
    std::string builder;
    TERM(t, 10, 3);
    wisp::vt::PageList *pages = &t.screens.active->pages;

    const size_t first_page_rows = pages->pages.first->capacity().rows;

    for (size_t i = 0; i < first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("hello world test");

    ASSERT_TRUE(pages->pages.first != pages->pages.last);

    /* Format with unwrap - should join the wrapped lines */
    PinMap::Map pin_map;
    Options opts(Format::plain, true);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    const std::string output = trimStart(builder, "\r\n");
    ASSERT_STR_EQ("hello world test", output.c_str());

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node first_node = pages->pages.first;
    PinMap::Node last_node = pages->pages.last;
    const size_t trimmed_count = builder.size() - output.size();

    for (size_t i = 0; i < trimmed_count; i++) ASSERT_TRUE(pin_map.get(i).value.node == first_node);
    for (size_t i = 0; i < 10; i++)
        ASSERT_TRUE(pin_map.get(trimmed_count + i).value.node == first_node);
    for (size_t i = 0; i < 6; i++)
        ASSERT_TRUE(pin_map.get(trimmed_count + 10 + i).value.node == last_node);
}

TEST(formatter, PageList_VT_spanning_two_pages) {
    std::string builder;
    TERM(t, 80, 24);
    wisp::vt::PageList *pages = &t.screens.active->pages;

    const size_t first_page_rows = pages->pages.first->capacity().rows;

    for (size_t i = 0; i < first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("\x1b[1mpage one");

    ASSERT_TRUE(pages->pages.first == pages->pages.last);

    t_s.nextSlice("\r\n");
    ASSERT_TRUE(pages->pages.first != pages->pages.last);

    /* New content is still styled */
    t_s.nextSlice("page two");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    const std::string output = trimStart(builder, "\r\n");
    ASSERT_STR_EQ("\x1b[0m\x1b[1mpage one\x1b[0m\r\n\x1b[0m\x1b[1mpage two\x1b[0m", output.c_str());

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node first_node = pages->pages.first;
    PinMap::Node last_node = pages->pages.last;

    /* Just verify we have entries for both pages in the pin map */
    size_t first_count = 0;
    size_t last_count = 0;
    for (size_t byte_i = 0; byte_i < pin_map.count(); byte_i++) {
        const Pin pin = pin_map.get(byte_i).value;
        if (pin.node == first_node) first_count += 1;
        if (pin.node == last_node) last_count += 1;
    }
    ASSERT_TRUE(first_count > 0);
    ASSERT_TRUE(last_count > 0);
}

TEST(formatter, PageList_plain_with_x_offset_on_single_page) {
    LIST_SETUP(t, 80, 24, "hello world\r\ntest case\r\nfoo bar");

    PinMap::Node node = pages->pages.first;

    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.top_left = vt::Maybe<Pin>(pinAt(node, 0, 6));
    formatter.bottom_right = vt::Maybe<Pin>(pinAt(node, 2, 2));
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("world\ntest case\nfoo", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    for (size_t byte_i = 0; byte_i < pin_map.count(); byte_i++)
        ASSERT_TRUE(pin_map.get(byte_i).value.node == node);

    /* "world" starts at x=6, y=0 */
    for (size_t i = 0; i < 5; i++) {
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)(6 + i));
        ASSERT_TRUE(pin_map.get(i).value.y == 0);
    }
}


TEST(formatter, PageList_plain_with_x_offset_spanning_two_pages) {
    std::string builder;
    TERM(t, 80, 24);
    wisp::vt::PageList *pages = &t.screens.active->pages;

    const size_t first_page_rows = pages->pages.first->capacity().rows;

    /* Fill first page almost completely */
    for (size_t i = 0; i < first_page_rows - 1; i++) t_s.nextSlice("\r\n");
    t_s.nextSlice("hello world");

    /* Verify we're still on one page */
    ASSERT_TRUE(pages->pages.first == pages->pages.last);

    /* Push to second page */
    t_s.nextSlice("\r\n");
    ASSERT_TRUE(pages->pages.first != pages->pages.last);
    t_s.nextSlice("foo bar test");

    PinMap::Node first_node = pages->pages.first;
    PinMap::Node last_node = pages->pages.last;

    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.top_left = vt::Maybe<Pin>(pinAt(first_node, first_node->rows() - 1, 6));
    formatter.bottom_right = vt::Maybe<Pin>(pinAt(last_node, 1, 2));
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    const std::string output = trimStart(builder, "\n");
    ASSERT_STR_EQ("world\nfoo", output.c_str());

    ASSERT_TRUE(builder.size() == pin_map.count());
    const size_t trimmed_count = builder.size() - output.size();

    /* "world" (5 chars) from first page */
    for (size_t i = 0; i < 5; i++) {
        const size_t idx = trimmed_count + i;
        ASSERT_TRUE(pin_map.get(idx).value.node == first_node);
        ASSERT_TRUE(pin_map.get(idx).value.x == (vt::size::CellCountInt)(6 + i));
    }

    /* \n - maps to last node as it represents the transition to new page */
    ASSERT_TRUE(pin_map.get(trimmed_count + 5).value.node == last_node);

    /* "foo" (3 chars) from last page */
    for (size_t i = 0; i < 3; i++) {
        const size_t idx = trimmed_count + 6 + i;
        ASSERT_TRUE(pin_map.get(idx).value.node == last_node);
        ASSERT_TRUE(pin_map.get(idx).value.x == (vt::size::CellCountInt)i);
    }
}

TEST(formatter, PageList_plain_with_start_x_only) {
    LIST_SETUP(t, 80, 24, "hello world");

    PinMap::Node node = pages->pages.first;

    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.top_left = vt::Maybe<Pin>(pinAt(node, 0, 6));
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("world", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    for (size_t i = 0; i < 5; i++) {
        ASSERT_TRUE(pin_map.get(i).value.node == node);
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)(6 + i));
        ASSERT_TRUE(pin_map.get(i).value.y == 0);
    }
}

TEST(formatter, PageList_plain_with_end_x_only) {
    LIST_SETUP(t, 80, 24, "hello world\r\ntest");

    PinMap::Node node = pages->pages.first;

    PinMap::Map pin_map;
    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.bottom_right = vt::Maybe<Pin>(pinAt(node, 1, 2));
    setPinMap(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("hello world\ntes", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    /* "hello world" (11 chars) on y=0 */
    for (size_t i = 0; i < 11; i++) {
        ASSERT_TRUE(pin_map.get(i).value.node == node);
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(i).value.y == 0);
    }
    /* \n */
    ASSERT_TRUE(pin_map.get(11).value.node == node);
    /* "tes" (3 chars) on y=1 */
    for (size_t i = 0; i < 3; i++) {
        ASSERT_TRUE(pin_map.get(12 + i).value.node == node);
        ASSERT_TRUE(pin_map.get(12 + i).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(12 + i).value.y == 1);
    }
}

TEST(formatter, PageList_plain_rectangle_basic) {
    std::string builder;
    TERM(t, 30, 5);

    t_s.nextSlice("Lorem ipsum dolor\r\n");
    t_s.nextSlice("sit amet, consectetur\r\n");
    t_s.nextSlice("adipiscing elit, sed do\r\n");
    t_s.nextSlice("eiusmod tempor incididunt\r\n");
    t_s.nextSlice("ut labore et dolore");

    wisp::vt::PageList *pages = &t.screens.active->pages;

    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.top_left = pages->pin(vt::point::Point::screen(2, 1));
    ASSERT_TRUE(formatter.top_left.has);
    formatter.bottom_right = pages->pin(vt::point::Point::screen(6, 3));
    ASSERT_TRUE(formatter.bottom_right.has);
    formatter.rectangle = true;

    formatter.format(&builder);
    EXPECT_STR("t ame\n"
               "ipisc\n"
               "usmod",
               builder);
}

TEST(formatter, PageList_plain_rectangle_with_EOL) {
    std::string builder;
    TERM(t, 30, 5);

    t_s.nextSlice("Lorem ipsum dolor\r\n");
    t_s.nextSlice("sit amet, consectetur\r\n");
    t_s.nextSlice("adipiscing elit, sed do\r\n");
    t_s.nextSlice("eiusmod tempor incididunt\r\n");
    t_s.nextSlice("ut labore et dolore");

    wisp::vt::PageList *pages = &t.screens.active->pages;

    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.top_left = pages->pin(vt::point::Point::screen(12, 0));
    ASSERT_TRUE(formatter.top_left.has);
    formatter.bottom_right = pages->pin(vt::point::Point::screen(26, 4));
    ASSERT_TRUE(formatter.bottom_right.has);
    formatter.rectangle = true;

    formatter.format(&builder);
    EXPECT_STR("dolor\n"
               "nsectetur\n"
               "lit, sed do\n"
               "or incididunt\n"
               " dolore",
               builder);
}

TEST(formatter, PageList_plain_rectangle_more_complex_with_breaks) {
    std::string builder;
    TERM(t, 30, 8);

    t_s.nextSlice("Lorem ipsum dolor\r\n");
    t_s.nextSlice("sit amet, consectetur\r\n");
    t_s.nextSlice("adipiscing elit, sed do\r\n");
    t_s.nextSlice("eiusmod tempor incididunt\r\n");
    t_s.nextSlice("ut labore et dolore\r\n");
    t_s.nextSlice("\r\n");
    t_s.nextSlice("magna aliqua. Ut enim\r\n");
    t_s.nextSlice("ad minim veniam, quis");

    wisp::vt::PageList *pages = &t.screens.active->pages;

    Options opts(Format::plain);
    PageListFormatter formatter = PageListFormatter::init(pages, &opts);
    formatter.top_left = pages->pin(vt::point::Point::screen(11, 2));
    ASSERT_TRUE(formatter.top_left.has);
    formatter.bottom_right = pages->pin(vt::point::Point::screen(26, 7));
    ASSERT_TRUE(formatter.bottom_right.has);
    formatter.rectangle = true;

    formatter.format(&builder);
    EXPECT_STR("elit, sed do\n"
               "por incididunt\n"
               "t dolore\n"
               "\n"
               "a. Ut enim\n"
               "niam, quis",
               builder);
}


/* ─── TerminalFormatter ────────────────────────────────────────────────── */

typedef fmt::TerminalFormatter TerminalFormatter;

/* Wisp: `formatter.pin_map = .{ .alloc = alloc, .map = &map }`. */
static void setPinMapT(TerminalFormatter *f, PinMap::Map *map) {
    PinMap pm;
    pm.map = map;
    f->pin_map = vt::Maybe<PinMap>(pm);
}

/* Wisp: `.{ .selection = .init(tl, br, false) }` */
static void setSelection(TerminalFormatter *f, vt::Terminal &t, unsigned x1, unsigned y1,
                         unsigned x2, unsigned y2) {
    const vt::Maybe<Pin> tl = t.screens.active->pages.pin(vt::point::Point::active(
        (vt::size::CellCountInt)x1, y1));
    const vt::Maybe<Pin> br = t.screens.active->pages.pin(vt::point::Point::active(
        (vt::size::CellCountInt)x2, y2));
    f->content.tag = fmt::ScreenFormatter::Content::Tag::selection;
    f->content.selection =
        vt::Maybe<vt::Selection>(vt::Selection::init(tl.value, br.value, false));
}

TEST(formatter, TerminalFormatter_plain_no_selection) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("hello\r\nworld");

    Options opts(Format::plain);
    const TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.format(&builder);
    EXPECT_STR("hello\nworld", builder);
}

TEST(formatter, TerminalFormatter_vt_with_palette) {
    std::string builder;
    TERM(t, 80, 24);

    /* Modify some palette colors using VT sequences */
    t_s.nextSlice("\x1b]4;0;rgb:12/34/56\x1b\\");
    t_s.nextSlice("\x1b]4;1;rgb:ab/cd/ef\x1b\\");
    t_s.nextSlice("\x1b]4;255;rgb:ff/00/ff\x1b\\");
    t_s.nextSlice("test");

    Options opts(Format::vt);
    const TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.format(&builder);

    /* Create a second terminal and apply the output */
    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify the palettes match */
    ASSERT_TRUE(t.colors.palette.current.colors[0].eql(t2.colors.palette.current.colors[0]));
    ASSERT_TRUE(t.colors.palette.current.colors[1].eql(t2.colors.palette.current.colors[1]));
    ASSERT_TRUE(t.colors.palette.current.colors[255].eql(t2.colors.palette.current.colors[255]));
}

TEST(formatter, TerminalFormatter_with_selection) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("line1\r\nline2\r\nline3");

    Options opts(Format::plain);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    setSelection(&formatter, t, 0, 1, 4, 1);

    formatter.format(&builder);
    EXPECT_STR("line2", builder);
}

TEST(formatter, TerminalFormatter_plain_with_pin_map) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("hello, world");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    setPinMapT(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("hello, world", builder);

    /* Verify pin map */
    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    for (size_t i = 0; i < builder.size(); i++) {
        const Pin got = pin_map.get(i).value;
        ASSERT_TRUE(got.node == node);
        ASSERT_TRUE(got.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(got.y == 0);
    }
}

TEST(formatter, TerminalFormatter_plain_multiline_with_pin_map) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("hello\r\nworld");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    setPinMapT(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("hello\nworld", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    /* "hello" (5 chars) */
    for (size_t i = 0; i < 5; i++) {
        ASSERT_TRUE(pin_map.get(i).value.node == node);
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(i).value.y == 0);
    }
    /* "\n" maps to end of first line */
    ASSERT_TRUE(pin_map.get(5).value.node == node);
    /* "world" (5 chars) */
    for (size_t i = 0; i < 5; i++) {
        const size_t idx = 6 + i;
        ASSERT_TRUE(pin_map.get(idx).value.node == node);
        ASSERT_TRUE(pin_map.get(idx).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(idx).value.y == 1);
    }
}

TEST(formatter, TerminalFormatter_vt_with_palette_and_pin_map) {
    std::string builder;
    TERM(t, 80, 24);

    /* Modify some palette colors using VT sequences */
    t_s.nextSlice("\x1b]4;0;rgb:12/34/56\x1b\\");
    t_s.nextSlice("test");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    setPinMapT(&formatter, &pin_map);

    formatter.format(&builder);

    /* Verify pin map - palette bytes should be mapped to top left */
    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    for (size_t i = 0; i < builder.size(); i++) ASSERT_TRUE(pin_map.get(i).value.node == node);
}

TEST(formatter, TerminalFormatter_with_selection_and_pin_map) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("line1\r\nline2\r\nline3");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    setSelection(&formatter, t, 0, 1, 4, 1);
    setPinMapT(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("line2", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    /* "line2" (5 chars) from row 1 */
    for (size_t i = 0; i < 5; i++) {
        ASSERT_TRUE(pin_map.get(i).value.node == node);
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(i).value.y == 1);
    }
}


/* ─── ScreenFormatter ──────────────────────────────────────────────────── */

typedef fmt::ScreenFormatter ScreenFormatter;

static void setPinMapS(ScreenFormatter *f, PinMap::Map *map) {
    PinMap pm;
    pm.map = map;
    f->pin_map = vt::Maybe<PinMap>(pm);
}

static void setSelectionS(ScreenFormatter *f, vt::Terminal &t, unsigned x1, unsigned y1,
                          unsigned x2, unsigned y2) {
    const vt::Maybe<Pin> tl =
        t.screens.active->pages.pin(vt::point::Point::active((vt::size::CellCountInt)x1, y1));
    const vt::Maybe<Pin> br =
        t.screens.active->pages.pin(vt::point::Point::active((vt::size::CellCountInt)x2, y2));
    f->content.tag = ScreenFormatter::Content::Tag::selection;
    f->content.selection = vt::Maybe<vt::Selection>(vt::Selection::init(tl.value, br.value, false));
}

TEST(formatter, Screen_plain_single_line) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("hello, world");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("hello, world", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    for (size_t i = 0; i < builder.size(); i++) {
        const Pin got = pin_map.get(i).value;
        ASSERT_TRUE(got.node == node);
        ASSERT_TRUE(got.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(got.y == 0);
    }
}

TEST(formatter, Screen_plain_multiline) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("hello\r\nworld");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("hello\nworld", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    /* "hello" (5 chars) */
    for (size_t i = 0; i < 5; i++) {
        ASSERT_TRUE(pin_map.get(i).value.node == node);
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(i).value.y == 0);
    }
    /* "\n" maps to end of first line */
    ASSERT_TRUE(pin_map.get(5).value.node == node);
    /* "world" (5 chars) */
    for (size_t i = 0; i < 5; i++) {
        const size_t idx = 6 + i;
        ASSERT_TRUE(pin_map.get(idx).value.node == node);
        ASSERT_TRUE(pin_map.get(idx).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(idx).value.y == 1);
    }
}

TEST(formatter, Screen_plain_with_selection) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("line1\r\nline2\r\nline3");

    PinMap::Map pin_map;
    Options opts(Format::plain);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    setSelectionS(&formatter, t, 0, 1, 4, 1);
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);
    EXPECT_STR("line2", builder);

    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    /* "line2" (5 chars) from row 1 */
    for (size_t i = 0; i < 5; i++) {
        ASSERT_TRUE(pin_map.get(i).value.node == node);
        ASSERT_TRUE(pin_map.get(i).value.x == (vt::size::CellCountInt)i);
        ASSERT_TRUE(pin_map.get(i).value.y == 1);
    }
}

TEST(formatter, Screen_vt_with_cursor_position) {
    std::string builder;
    TERM(t, 80, 24);

    /* Position cursor at a specific location */
    t_s.nextSlice("hello\r\nworld");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    formatter.extra.cursor = true;
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);

    /* Create a second terminal and apply the output */
    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify cursor positions match */
    ASSERT_TRUE(t.screens.active->cursor.x == t2.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.y == t2.screens.active->cursor.y);

    /* Verify pin map - the extras should be mapped to the last pin */
    ASSERT_TRUE(builder.size() == pin_map.count());
    PinMap::Node node = t.screens.active->pages.pages.first;
    const size_t content_len = strlen("hello\r\nworld");
    /* Content bytes map to their positions */
    for (size_t i = 0; i < content_len; i++) ASSERT_TRUE(pin_map.get(i).value.node == node);
    /* Extra bytes (cursor position) map to last content pin */
    for (size_t i = content_len; i < builder.size(); i++)
        ASSERT_TRUE(pin_map.get(i).value.node == node);
}

TEST(formatter, Terminal_vt_cursor_preserves_pending_wrap) {
    std::string builder;
    TERM(source, 4, 2);
    source_s.nextSlice("abcd");
    ASSERT_TRUE(source.screens.active->cursor.pending_wrap);

    PinMap::Map pin_map;
    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&source, opts);
    formatter.extra = TerminalFormatter::Extra::none();
    formatter.extra.screen.cursor = true;
    setPinMapT(&formatter, &pin_map);

    formatter.format(&builder);
    ASSERT_TRUE(builder.size() == pin_map.count());

    TERM(target, 4, 2);
    target_s.nextSlice(builder.data(), builder.size());
    ASSERT_TRUE(source.screens.active->cursor.pending_wrap ==
                target.screens.active->cursor.pending_wrap);

    source_s.nextSlice("X");
    target_s.nextSlice("X");

    const std::string source_contents = source.screens.active->dumpStringAlloc(vt::point::Point::screen());
    const std::string target_contents = target.screens.active->dumpStringAlloc(vt::point::Point::screen());
    ASSERT_STR_EQ(source_contents.c_str(), target_contents.c_str());
}


/* Wisp: the "apply the output to a second terminal" tail these tests share:
 * every byte of the output must map to the one page node. */
static bool allPinNodesAre(const PinMap::Map &m, size_t len, PinMap::Node node) {
    for (size_t i = 0; i < len; i++) {
        if (m.get(i).value.node != node) return false;
    }
    return true;
}

TEST(formatter, Screen_vt_with_style) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set some style attributes */
    t_s.nextSlice("\x1b[1;31mhello");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    formatter.extra.style = true;
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);

    /* Create a second terminal and apply the output */
    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify styles match */
    ASSERT_TRUE(t.screens.active->cursor.style.eql(t2.screens.active->cursor.style));

    ASSERT_TRUE(builder.size() == pin_map.count());
    ASSERT_TRUE(allPinNodesAre(pin_map, builder.size(), t.screens.active->pages.pages.first));
}

TEST(formatter, Screen_vt_with_hyperlink) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set a hyperlink */
    t_s.nextSlice("\x1b]8;;http://example.com\x1b\\hello");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    formatter.extra.hyperlink = true;
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify hyperlinks match */
    const bool has_link1 = t.screens.active->cursor.hyperlink != nullptr;
    const bool has_link2 = t2.screens.active->cursor.hyperlink != nullptr;
    ASSERT_TRUE(has_link1 == has_link2);
    if (has_link1) {
        const vt::hyperlink::Hyperlink *link1 = t.screens.active->cursor.hyperlink;
        const vt::hyperlink::Hyperlink *link2 = t2.screens.active->cursor.hyperlink;
        ASSERT_TRUE(link1->uri_len == link2->uri_len);
        ASSERT_TRUE(0 == memcmp(link1->uri, link2->uri, link1->uri_len));
    }

    ASSERT_TRUE(builder.size() == pin_map.count());
    ASSERT_TRUE(allPinNodesAre(pin_map, builder.size(), t.screens.active->pages.pages.first));
}

TEST(formatter, Screen_vt_with_protection) {
    std::string builder;
    TERM(t, 80, 24);

    /* Enable protection mode */
    t_s.nextSlice("\x1b[1\"qhello");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    formatter.extra.protection = true;
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify protection state matches */
    ASSERT_TRUE(t.screens.active->cursor.protected_ == t2.screens.active->cursor.protected_);

    ASSERT_TRUE(builder.size() == pin_map.count());
    ASSERT_TRUE(allPinNodesAre(pin_map, builder.size(), t.screens.active->pages.pages.first));
}

TEST(formatter, Screen_vt_with_kitty_keyboard) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set kitty keyboard flags (disambiguate + report_events = 3) */
    t_s.nextSlice("\x1b[=3;1uhello");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    formatter.extra.kitty_keyboard = true;
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify kitty keyboard state matches */
    ASSERT_TRUE(t.screens.active->kitty_keyboard.current().int_() ==
                t2.screens.active->kitty_keyboard.current().int_());

    ASSERT_TRUE(builder.size() == pin_map.count());
    ASSERT_TRUE(allPinNodesAre(pin_map, builder.size(), t.screens.active->pages.pages.first));
}

TEST(formatter, Screen_vt_with_charsets) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set G0 to DEC special and shift to G1 */
    t_s.nextSlice("\x1b(0\x0ehello");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    ScreenFormatter formatter = ScreenFormatter::init(t.screens.active, opts);
    formatter.extra.charsets = true;
    setPinMapS(&formatter, &pin_map);

    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify charset state matches */
    ASSERT_TRUE(t.screens.active->charset.gl == t2.screens.active->charset.gl);
    ASSERT_TRUE(t.screens.active->charset.gr == t2.screens.active->charset.gr);
    ASSERT_TRUE(t.screens.active->charset.charsets.get(vt::Screen::CharsetState::Slots::G0) ==
                t2.screens.active->charset.charsets.get(vt::Screen::CharsetState::Slots::G0));

    ASSERT_TRUE(builder.size() == pin_map.count());
    ASSERT_TRUE(allPinNodesAre(pin_map, builder.size(), t.screens.active->pages.pages.first));
}


TEST(formatter, Terminal_vt_with_scrolling_region) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set scrolling region: top=5, bottom=20 */
    t_s.nextSlice("\x1b[6;21rhello");

    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.extra.scrolling_region = true;
    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify scrolling regions match */
    ASSERT_TRUE(t.scrolling_region.top == t2.scrolling_region.top);
    ASSERT_TRUE(t.scrolling_region.bottom == t2.scrolling_region.bottom);
    ASSERT_TRUE(t.scrolling_region.left == t2.scrolling_region.left);
    ASSERT_TRUE(t.scrolling_region.right == t2.scrolling_region.right);
}

TEST(formatter, Terminal_vt_with_modes) {
    std::string builder;
    TERM(t, 80, 24);

    /* Enable some modes that differ from defaults */
    t_s.nextSlice("\x1b[?2004h"); /* Bracketed paste */
    t_s.nextSlice("\x1b[?1000h"); /* Mouse event normal */
    t_s.nextSlice("\x1b[?7l");    /* Disable wraparound (default is true) */
    t_s.nextSlice("hello");

    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.extra.modes = true;
    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify modes match */
    ASSERT_TRUE(t.modes.get(modes::Mode::bracketed_paste) ==
                t2.modes.get(modes::Mode::bracketed_paste));
    ASSERT_TRUE(t.modes.get(modes::Mode::mouse_event_normal) ==
                t2.modes.get(modes::Mode::mouse_event_normal));
    ASSERT_TRUE(t.modes.get(modes::Mode::wraparound) == t2.modes.get(modes::Mode::wraparound));
}

TEST(formatter, Terminal_vt_with_tabstops) {
    std::string builder;
    TERM(t, 80, 24);

    /* Clear all tabs and set custom tabstops */
    t_s.nextSlice("\x1b[3g");      /* Clear all tabs */
    t_s.nextSlice("\x1b[5G\x1bH");  /* Set tab at column 5 */
    t_s.nextSlice("\x1b[15G\x1bH"); /* Set tab at column 15 */
    t_s.nextSlice("\x1b[30G\x1bH"); /* Set tab at column 30 */
    t_s.nextSlice("\x1b[Hhello");

    PinMap::Map pin_map;
    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.extra.tabstops = true;
    formatter.extra.screen.cursor = true;
    setPinMapT(&formatter, &pin_map);
    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify tabstops match (columns are 0-indexed in the API) */
    ASSERT_TRUE(t.tabstops.get(4) == t2.tabstops.get(4));
    ASSERT_TRUE(t.tabstops.get(14) == t2.tabstops.get(14));
    ASSERT_TRUE(t.tabstops.get(29) == t2.tabstops.get(29));
    ASSERT_TRUE(t2.tabstops.get(4));   /* Column 5 (1-indexed) */
    ASSERT_TRUE(t2.tabstops.get(14));  /* Column 15 (1-indexed) */
    ASSERT_TRUE(t2.tabstops.get(29));  /* Column 30 (1-indexed) */
    ASSERT_TRUE(!t2.tabstops.get(8));  /* Not a tab */

    /* Tabstop serialization must not offset the screen contents. */
    {
        static const char expected[] = "hello";
        for (size_t col = 0; col < 5; col++) {
            const vt::Maybe<wisp::vt::PageList::Cell> cell = t2.screens.active->pages.getCell(
                vt::point::Point::screen((vt::size::CellCountInt)col, 0));
            ASSERT_TRUE(cell.has);
            ASSERT_TRUE((uint32_t)expected[col] == cell.value.cell->codepoint());
        }
    }

    /* Emitting tabstops moves the cursor to each configured column. When
     * cursor state is included, it must be restored afterwards. */
    ASSERT_TRUE(t.screens.active->cursor.x == t2.screens.active->cursor.x);
    ASSERT_TRUE(t.screens.active->cursor.y == t2.screens.active->cursor.y);

    /* Verify the reordered terminal state is still represented in the map. */
    ASSERT_TRUE(builder.size() == pin_map.count());
}

TEST(formatter, Terminal_vt_with_keyboard_modes) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set modify other keys mode 2 */
    t_s.nextSlice("\x1b[>4;2m");
    t_s.nextSlice("hello");

    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.extra.keyboard = true;
    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify keyboard mode matches */
    ASSERT_TRUE(t.flags.modify_other_keys_2 == t2.flags.modify_other_keys_2);
    ASSERT_TRUE(t2.flags.modify_other_keys_2);
}

TEST(formatter, Terminal_vt_with_pwd) {
    std::string builder;
    TERM(t, 80, 24);

    /* Set pwd using OSC 7 */
    t_s.nextSlice("\x1b]7;file://host/home/user\x1b\\hello");

    Options opts(Format::vt);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.extra.pwd = true;
    formatter.format(&builder);

    TERM(t2, 80, 24);
    t2_s.nextSlice(builder.data(), builder.size());

    /* Verify pwd matches */
    ASSERT_TRUE(t.pwd.len == t2.pwd.len);
    ASSERT_TRUE(0 == memcmp(t.pwd.items, t2.pwd.items, t.pwd.len));
}


/* ─── html ─────────────────────────────────────────────────────────────── */

#define HTML_WRAP "<div style=\"font-family: monospace; white-space: pre;\">"

TEST(formatter, Page_html_with_multiple_styles) {
    /* Set bold, then italic, then reset */
    PAGE_SETUP(t, 80, 24, "\x1b[1mbold\x1b[3mitalic\x1b[0mnormal");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<div style=\"display: inline;font-weight: bold;\">bold</div>"
                         "<div style=\"display: inline;font-weight: bold;font-style: italic;\">"
                         "italic</div>"
                         "normal"
                         "</div>",
               builder);
}

TEST(formatter, Page_html_plain_text) {
    PAGE_SETUP(t, 80, 24, "hello, world");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    /* Plain text without styles should be wrapped in monospace div */
    EXPECT_STR(HTML_WRAP "hello, world</div>", builder);
}

TEST(formatter, Page_html_with_colors) {
    /* Set red foreground, blue background */
    PAGE_SETUP(t, 80, 24, "\x1b[31;44mcolored");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<div style=\"display: inline;color: var(--vt-palette-1);"
                         "background-color: var(--vt-palette-4);\">colored</div>"
                         "</div>",
               builder);
}

TEST(formatter, TerminalFormatter_html_with_palette) {
    std::string builder;
    TERM(t, 80, 24);

    /* Modify some palette colors */
    t_s.nextSlice("\x1b]4;0;rgb:12/34/56\x1b\\");
    t_s.nextSlice("\x1b]4;1;rgb:ab/cd/ef\x1b\\");
    t_s.nextSlice("\x1b]4;255;rgb:ff/00/ff\x1b\\");
    t_s.nextSlice("test");

    Options opts(Format::html);
    TerminalFormatter formatter = TerminalFormatter::init(&t, opts);
    formatter.extra.palette = true;
    formatter.format(&builder);

    /* Verify palette CSS variables are emitted */
    ASSERT_TRUE(builder.find("<style>:root{") != std::string::npos);
    ASSERT_TRUE(builder.find("--vt-palette-0: #123456;") != std::string::npos);
    ASSERT_TRUE(builder.find("--vt-palette-1: #abcdef;") != std::string::npos);
    ASSERT_TRUE(builder.find("--vt-palette-255: #ff00ff;") != std::string::npos);
    ASSERT_TRUE(builder.find("}</style>") != std::string::npos);
    ASSERT_TRUE(builder.find("test") != std::string::npos);
}

TEST(formatter, Page_html_with_background_and_foreground_colors) {
    PAGE_SETUP(t, 80, 24, "hello");

    Options opts(Format::html);
    opts.background = rgb(0x12, 0x34, 0x56);
    opts.foreground = rgb(0xab, 0xcd, 0xef);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR("<div style=\"font-family: monospace; white-space: pre;"
               "background-color: #123456;color: #abcdef;\">hello</div>",
               builder);
}

TEST(formatter, Page_html_with_escaping) {
    PAGE_SETUP(t, 80, 24, "<tag>&\"'text");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "&lt;tag&gt;&amp;&quot;&#39;text</div>", builder);

    /* Verify point map length matches output */
    ASSERT_TRUE(builder.size() == point_map.size());

    /* Opening wrapper div */
    const size_t wrapper_start_len = strlen(HTML_WRAP);
    for (size_t i = 0; i < wrapper_start_len; i++) ASSERT_TRUE(coordEq(point_map[i], 0, 0));

    /* Verify each character maps correctly, accounting for escaping */
    const size_t offset = wrapper_start_len;
    /* < (4 bytes: &lt;) -> x=0 */
    for (size_t i = 0; i < 4; i++) ASSERT_TRUE(coordEq(point_map[offset + i], 0, 0));
    /* t (1 byte) -> x=1 */
    ASSERT_TRUE(coordEq(point_map[offset + 4], 1, 0));
    /* a (1 byte) -> x=2 */
    ASSERT_TRUE(coordEq(point_map[offset + 5], 2, 0));
    /* g (1 byte) -> x=3 */
    ASSERT_TRUE(coordEq(point_map[offset + 6], 3, 0));
    /* > (4 bytes: &gt;) -> x=4 */
    for (size_t i = 0; i < 4; i++) ASSERT_TRUE(coordEq(point_map[offset + 7 + i], 4, 0));
    /* & (5 bytes: &amp;) -> x=5 */
    for (size_t i = 0; i < 5; i++) ASSERT_TRUE(coordEq(point_map[offset + 11 + i], 5, 0));
    /* " (6 bytes: &quot;) -> x=6 */
    for (size_t i = 0; i < 6; i++) ASSERT_TRUE(coordEq(point_map[offset + 16 + i], 6, 0));
    /* ' (5 bytes: &#39;) -> x=7 */
    for (size_t i = 0; i < 5; i++) ASSERT_TRUE(coordEq(point_map[offset + 22 + i], 7, 0));
    /* t (1 byte) -> x=8 */
    ASSERT_TRUE(coordEq(point_map[offset + 27], 8, 0));
    /* e (1 byte) -> x=9 */
    ASSERT_TRUE(coordEq(point_map[offset + 28], 9, 0));
    /* x (1 byte) -> x=10 */
    ASSERT_TRUE(coordEq(point_map[offset + 29], 10, 0));
    /* t (1 byte) -> x=11 */
    ASSERT_TRUE(coordEq(point_map[offset + 30], 11, 0));
}

TEST(formatter, Page_html_with_unicode_as_numeric_entities) {
    /* Box drawing characters that caused issue #9426 */
    PAGE_SETUP(t, 80, 24, "\xE2\x95\xB0\xE2\x94\x80 \xE2\x9D\xAF");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    /* Expected: box drawing chars as numeric entities
     * ╰ = U+2570 = 9584, ─ = U+2500 = 9472, ❯ = U+276F = 10095 */
    EXPECT_STR(HTML_WRAP "&#9584;&#9472; &#10095;</div>", builder);
}

TEST(formatter, Page_html_trailing_blank_lines) {
    PAGE_SETUP(t, 80, 24, "hello\r\nworld\r\n\r\n");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    const PageFormatter::TrailingState state = formatter.formatWithState(&builder);
    /* The closing div behaves as a newline */
    ASSERT_TRUE((size_t)(page->size.rows - 2) == state.rows);
    EXPECT_STR(HTML_WRAP "hello\nworld</div>", builder);
}

TEST(formatter, Page_html_ascii_characters_unchanged) {
    PAGE_SETUP(t, 80, 24, "hello world");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    /* ASCII should be emitted directly */
    EXPECT_STR(HTML_WRAP "hello world</div>", builder);
}

TEST(formatter, Page_html_mixed_ascii_and_unicode) {
    PAGE_SETUP(t, 80, 24, "test \xE2\x95\xB0\xE2\x94\x80\xE2\x9D\xAF ok");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    /* Mix of ASCII and Unicode entities */
    EXPECT_STR(HTML_WRAP "test &#9584;&#9472;&#10095; ok</div>", builder);
}

TEST(formatter, Page_VT_with_palette_option_emits_RGB) {
    /* Set a custom palette color and use it */
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("\x1b]4;1;rgb:ab/cd/ef\x1b\\");
    t_s.nextSlice("\x1b[31mred");
    const fmt::Page *page = singlePage(t);
    ASSERT_TRUE(page != nullptr);

    /* Without palette option - should emit palette index */
    {
        builder.clear();
        Options opts(Format::vt);
        PageFormatter formatter = PageFormatter::init(page, &opts);
        formatter.format(&builder);
        EXPECT_STR("\x1b[0m\x1b[38;5;1mred\x1b[0m", builder);
    }

    /* With palette option - should emit RGB directly */
    {
        builder.clear();
        Options opts(Format::vt);
        opts.palette = &t.colors.palette.current;
        PageFormatter formatter = PageFormatter::init(page, &opts);
        formatter.format(&builder);
        EXPECT_STR("\x1b[0m\x1b[38;2;171;205;239mred\x1b[0m", builder);
    }
}

TEST(formatter, Page_html_with_palette_option_emits_RGB) {
    std::string builder;
    TERM(t, 80, 24);
    t_s.nextSlice("\x1b]4;1;rgb:ab/cd/ef\x1b\\");
    t_s.nextSlice("\x1b[31mred");
    const fmt::Page *page = singlePage(t);
    ASSERT_TRUE(page != nullptr);

    /* Without palette option - should emit CSS variable */
    {
        builder.clear();
        Options opts(Format::html);
        PageFormatter formatter = PageFormatter::init(page, &opts);
        formatter.format(&builder);
        EXPECT_STR(HTML_WRAP "<div style=\"display: inline;color: var(--vt-palette-1);\">red</div>"
                             "</div>",
                   builder);
    }

    /* With palette option - should emit RGB directly */
    {
        builder.clear();
        Options opts(Format::html);
        opts.palette = &t.colors.palette.current;
        PageFormatter formatter = PageFormatter::init(page, &opts);
        formatter.format(&builder);
        EXPECT_STR(HTML_WRAP "<div style=\"display: inline;color: rgb(171, 205, 239);\">red</div>"
                             "</div>",
                   builder);
    }
}

TEST(formatter, Page_VT_style_reset_properly_closes_styles) {
    /* Set bold, then reset with SGR 0 */
    PAGE_SETUP(t, 80, 24, "\x1b[1mbold\x1b[0mnormal");

    Options opts(Format::vt);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    /* The reset should properly close the bold style */
    EXPECT_STR("\x1b[0m\x1b[1mbold\x1b[0mnormal", builder);
}


/* ─── codepoint_map ────────────────────────────────────────────────────── */

/* Wisp: `try map.append(alloc, .{ .range = .{ a, b },
 * .replacement = .{ .codepoint = c } })` */
static void mapCodepoint(std::vector<fmt::CodepointMap> *map, uint32_t lo, uint32_t hi,
                         uint32_t replacement) {
    fmt::CodepointMap m;
    m.range[0] = lo;
    m.range[1] = hi;
    m.replacement.tag = fmt::CodepointMap::Replacement::Tag::codepoint;
    m.replacement.codepoint = replacement;
    map->push_back(m);
}

static void mapString(std::vector<fmt::CodepointMap> *map, uint32_t lo, uint32_t hi,
                      const char *replacement) {
    fmt::CodepointMap m;
    m.range[0] = lo;
    m.range[1] = hi;
    m.replacement.tag = fmt::CodepointMap::Replacement::Tag::string;
    m.replacement.codepoint = 0;
    m.replacement.string = replacement;
    map->push_back(m);
}

TEST(formatter, Page_codepoint_map_single_replacement) {
    PAGE_SETUP(t, 80, 24, "hello world");

    /* Replace 'o' with 'x' */
    Options opts(Format::plain);
    mapCodepoint(&opts.codepoint_map, 'o', 'o', 'x');
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("hellx wxrld", builder);

    /* Verify point map - each output byte should map to original cell position */
    ASSERT_TRUE(builder.size() == point_map.size());
    /* "hello world" -> "hellx wxrld" */
    ASSERT_TRUE(pointRunIs(point_map, 0, 11, 0, 0));
}

TEST(formatter, Page_codepoint_map_conflicting_replacement_prefers_last) {
    PAGE_SETUP(t, 80, 24, "hello");

    /* Replace 'o' with 'x', then with 'y' - should prefer last */
    Options opts(Format::plain);
    mapCodepoint(&opts.codepoint_map, 'o', 'o', 'x');
    mapCodepoint(&opts.codepoint_map, 'o', 'o', 'y');
    PageFormatter formatter = PageFormatter::init(page, &opts);

    formatter.format(&builder);
    EXPECT_STR("helly", builder);
}

TEST(formatter, Page_codepoint_map_replace_with_string) {
    PAGE_SETUP(t, 80, 24, "hello");

    /* Replace 'o' with a multi-byte string */
    Options opts(Format::plain);
    mapString(&opts.codepoint_map, 'o', 'o', "XYZ");
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("hellXYZ", builder);

    /* Verify point map - string replacements should all map to the original cell */
    ASSERT_TRUE(builder.size() == point_map.size());
    ASSERT_TRUE(pointRunIs(point_map, 0, 4, 0, 0));
    /* All bytes of the replacement string "XYZ" should point to position 4 */
    ASSERT_TRUE(coordEq(point_map[4], 4, 0)); /* X */
    ASSERT_TRUE(coordEq(point_map[5], 4, 0)); /* Y */
    ASSERT_TRUE(coordEq(point_map[6], 4, 0)); /* Z */
}

TEST(formatter, Page_codepoint_map_range_replacement) {
    PAGE_SETUP(t, 80, 24, "abcdefg");

    /* Replace 'b' through 'e' with 'X' */
    Options opts(Format::plain);
    mapCodepoint(&opts.codepoint_map, 'b', 'e', 'X');
    PageFormatter formatter = PageFormatter::init(page, &opts);

    formatter.format(&builder);
    EXPECT_STR("aXXXXfg", builder);
}

TEST(formatter, Page_codepoint_map_multiple_ranges) {
    PAGE_SETUP(t, 80, 24, "hello world");

    /* Replace 'a'-'m' with 'A' and 'n'-'z' with 'Z' */
    Options opts(Format::plain);
    mapCodepoint(&opts.codepoint_map, 'a', 'm', 'A');
    mapCodepoint(&opts.codepoint_map, 'n', 'z', 'Z');
    PageFormatter formatter = PageFormatter::init(page, &opts);

    formatter.format(&builder);
    EXPECT_STR("AAAAZ ZZZAA", builder);
}

TEST(formatter, Page_codepoint_map_unicode_replacement) {
    PAGE_SETUP(t, 80, 24, "hello \xE2\x9A\xA1 world");

    /* Replace lightning bolt with fire emoji */
    Options opts(Format::plain);
    mapString(&opts.codepoint_map, 0x26A1, 0x26A1, "\xF0\x9F\x94\xA5");
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);
    EXPECT_STR("hello \xF0\x9F\x94\xA5 world", builder);

    ASSERT_TRUE(builder.size() == point_map.size());
    /* Note: ⚡ is a wide character occupying cells 6-7 */
    ASSERT_TRUE(pointRunIs(point_map, 0, 6, 0, 0));
    /* 🔥 is 4 UTF-8 bytes, all should map to cell 6 (where ⚡ was) */
    const size_t fire_start = 6; /* "hello " is 6 bytes */
    for (size_t i = 0; i < 4; i++) ASSERT_TRUE(coordEq(point_map[fire_start + i], 6, 0));
    /* " world" follows */
    ASSERT_TRUE(pointRunIs(point_map, fire_start + 4, 6, 8, 0));
}

TEST(formatter, Page_codepoint_map_with_styled_formats) {
    PAGE_SETUP(t, 10, 24, "\x1b[31mred text\x1b[0m");

    /* Replace 'e' with 'X' in styled text */
    Options opts(Format::vt);
    mapCodepoint(&opts.codepoint_map, 'e', 'e', 'X');
    PageFormatter formatter = PageFormatter::init(page, &opts);

    formatter.format(&builder);
    /* Should preserve styles while replacing text
     * "red text" becomes "rXd tXxt"
     * VT format uses \x1b[38;5;1m for palette color 1 */
    EXPECT_STR("\x1b[0m\x1b[38;5;1mrXd tXxt\x1b[0m", builder);
}

TEST(formatter, Page_codepoint_map_empty_map) {
    PAGE_SETUP(t, 80, 24, "hello world");

    /* Empty map should not change anything */
    Options opts(Format::plain);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    formatter.format(&builder);
    EXPECT_STR("hello world", builder);
}

TEST(formatter, Page_VT_background_color_on_trailing_blank_cells) {
    /* This test reproduces a bug where trailing cells with background color
     * but no text are emitted as plain spaces without SGR sequences.
     * This causes TUIs like htop to lose background colors on rehydration. */
    std::string builder;
    TERM(t, 20, 5);

    /* Simulate a TUI row: "CPU:" with text, then trailing cells with red background
     * to end of line (no text after the colored region).
     * \x1b[41m sets red background, then EL fills rest of row with that bg. */
    t_s.nextSlice("CPU:\x1b[41m\x1b[K");
    /* Reset colors and move to next line with different content */
    t_s.nextSlice("\x1b[0m\r\nline2");

    const fmt::Page *page = singlePage(t);
    ASSERT_TRUE(page != nullptr);

    Options opts(Format::vt);
    opts.trim = false; /* Don't trim so we can see the trailing behavior */
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);

    /* The output should preserve the red background SGR for trailing cells on line 1.
     * The red background should appear BEFORE the newline, not after. */
    const size_t crlf_pos = builder.find("\r\n");
    ASSERT_TRUE(crlf_pos != std::string::npos);

    /* Check that red background (48;5;1) appears BEFORE the newline (on line 1) */
    const std::string line1 = builder.substr(0, crlf_pos);
    const bool has_red_bg_line1 =
        line1.find("\x1b[41m") != std::string::npos ||
        line1.find("\x1b[48;5;1m") != std::string::npos;
    ASSERT_TRUE(has_red_bg_line1);
}

TEST(formatter, Page_HTML_with_hyperlinks) {
    /* Start a hyperlink, write some text, end it */
    PAGE_SETUP(t, 80, 24, "\x1b]8;;https://example.com\x1b\\link text\x1b]8;;\x1b\\ normal");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<a href=\"https://example.com\">link text</a> normal"
                         "</div>",
               builder);
}

TEST(formatter, Page_HTML_with_multiple_hyperlinks) {
    std::string builder;
    TERM(t, 80, 24);

    /* Two different hyperlinks */
    t_s.nextSlice("\x1b]8;;https://first.com\x1b\\first\x1b]8;;\x1b\\ ");
    t_s.nextSlice("\x1b]8;;https://second.com\x1b\\second\x1b]8;;\x1b\\");

    const fmt::Page *page = singlePage(t);
    ASSERT_TRUE(page != nullptr);

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<a href=\"https://first.com\">first</a>"
                         " "
                         "<a href=\"https://second.com\">second</a>"
                         "</div>",
               builder);
}

TEST(formatter, Page_HTML_with_hyperlink_escaping) {
    /* URL with special characters that need escaping */
    PAGE_SETUP(t, 80, 24, "\x1b]8;;https://example.com?a=1&b=2\x1b\\link\x1b]8;;\x1b\\");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<a href=\"https://example.com?a=1&amp;b=2\">link</a>"
                         "</div>",
               builder);
}

TEST(formatter, Page_HTML_with_styled_hyperlink) {
    /* Bold hyperlink */
    PAGE_SETUP(t, 80, 24, "\x1b]8;;https://example.com\x1b\\\x1b[1mbold link\x1b[0m\x1b]8;;\x1b\\");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<div style=\"display: inline;font-weight: bold;\">"
                         "<a href=\"https://example.com\">bold link</div></a>"
                         "</div>",
               builder);
}

TEST(formatter, Page_HTML_hyperlink_closes_style_before_anchor) {
    /* Styled hyperlink followed by plain text */
    PAGE_SETUP(t, 80, 24, "\x1b]8;;https://example.com\x1b\\\x1b[1mbold\x1b[0m plain");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);
    formatter.format(&builder);
    EXPECT_STR(HTML_WRAP "<div style=\"display: inline;font-weight: bold;\">"
                         "<a href=\"https://example.com\">bold</div> plain</a>"
                         "</div>",
               builder);
}

TEST(formatter, Page_HTML_hyperlink_point_map_maps_closing_to_previous_cell) {
    PAGE_SETUP(t, 80, 24, "\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\ normal");

    Options opts(Format::html);
    PageFormatter formatter = PageFormatter::init(page, &opts);

    std::vector<Coordinate> point_map;
    setPointMap(&formatter, &point_map);

    formatter.format(&builder);

    static const char expected_output[] = HTML_WRAP
        "<a href=\"https://example.com\">link</a> normal"
        "</div>";
    EXPECT_STR(expected_output, builder);
    ASSERT_TRUE(strlen(expected_output) == point_map.size());

    /* The </a> closing tag bytes should all map to the last cell of the link */
    const size_t closing_idx = std::string(expected_output).find("</a>");
    ASSERT_TRUE(closing_idx != std::string::npos);
    const Coordinate expected_coord = point_map[closing_idx - 1];
    for (size_t i = closing_idx; i < closing_idx + 4; i++)
        ASSERT_TRUE(coordEq(point_map[i], expected_coord.x, expected_coord.y));
}

TEST(formatter, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
