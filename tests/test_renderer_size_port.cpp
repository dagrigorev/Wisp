/* Transliterated from the test blocks in Ghostty src/renderer/size.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "../src/renderer/size.hpp"

using namespace wisp::renderer::size;

static Size makeSize(uint32_t sw, uint32_t sh, uint32_t cw, uint32_t ch) {
    Size s;
    s.screen = {sw, sh};
    s.cell = {cw, ch};
    s.padding = Padding();
    return s;
}

TEST(renderer_size, Size_balancePadding_equal_distributes_whitespace_equally) {
    /* screen=1050x850, cell=10x20, explicit=4 each side
     * grid: (1050-8)/10=104 cols, (850-8)/20=42 rows
     * leftover: 1050-1040=10 horizontal, 850-840=10 vertical
     * balanced: left=right=5, top=bottom=5 */
    Size size = makeSize(1050, 850, 10, 20);
    Padding explicit_;
    explicit_.top = 4;
    explicit_.bottom = 4;
    explicit_.left = 4;
    explicit_.right = 4;
    size.balancePadding(explicit_, PaddingBalance::equal);
    ASSERT_EQ((long)size.padding.left, (long)size.padding.right);
    ASSERT_EQ((long)size.padding.top, (long)size.padding.bottom);
    ASSERT_TRUE(size.padding.top > 0);
}

TEST(renderer_size, Size_balancePadding_true_shifts_excess_top_to_bottom) {
    /* screen=1090x1070, cell=20x40, explicit=0
     * grid: 1090/20=54 cols, 1070/40=26 rows
     * leftover: 1090-1080=10, 1070-1040=30
     * balanced: left=right=5, top=bottom=15
     * vshift cap: (0+0+20)/2=10, vshift=15-10=5
     * result: top=10, bottom=20 */
    Size size = makeSize(1090, 1070, 20, 40);
    size.balancePadding(Padding(), PaddingBalance::true_);
    ASSERT_EQ((long)size.padding.left, (long)size.padding.right);
    ASSERT_TRUE(size.padding.top < size.padding.bottom);
    ASSERT_EQ(10L, (long)size.padding.top);
    ASSERT_EQ(20L, (long)size.padding.bottom);
}

TEST(renderer_size, Padding_balanced_on_zero) {
    /* On some systems, our screen can be zero-sized for a bit, and we
     * don't want to end up with negative padding. */
    GridSize grid;
    grid.columns = 100;
    grid.rows = 37;
    const CellSize cell = {10, 20};
    const ScreenSize screen = {0, 0};
    const Padding padding = Padding::balanced(screen, grid, cell);
    ASSERT_TRUE(Padding().eql(padding));
}

TEST(renderer_size, GridSize_update_exact) {
    GridSize grid;
    grid.update({100, 40}, {5, 10});

    ASSERT_EQ(20L, (long)grid.columns);
    ASSERT_EQ(4L, (long)grid.rows);
}

TEST(renderer_size, GridSize_update_rounding) {
    GridSize grid;
    grid.update({20, 40}, {6, 15});

    ASSERT_EQ(3L, (long)grid.columns);
    ASSERT_EQ(2L, (long)grid.rows);
}

TEST(renderer_size, coordinate_conversion) {
    /* A size for testing purposes. Purposely easy to calculate numbers. */
    const Size test_size = makeSize(100, 100, 5, 10);

    /* Each pair is a test case of [expected, actual]. We only test
     * one-way conversion because conversion can be lossy due to clamping
     * and so on. */
    const Coordinate table[][2] = {
        {Coordinate::makeGrid(0, 0), Coordinate::makeSurface(0, 0)},
        {Coordinate::makeGrid(1, 0), Coordinate::makeSurface(6, 0)},
        {Coordinate::makeGrid(1, 1), Coordinate::makeSurface(6, 10)},
        {Coordinate::makeGrid(0, 0), Coordinate::makeSurface(-10, -10)},
        {Coordinate::makeGrid((GridSize::Unit)(test_size.grid().columns - 1),
                              (GridSize::Unit)(test_size.grid().rows - 1)),
         Coordinate::makeSurface(100000, 100000)},
    };

    for (const auto &pair : table) {
        const Coordinate expected = pair[0];
        const Coordinate actual = pair[1].convert(expected.tag, test_size);
        ASSERT_TRUE(expected == actual);
    }
}
