/* Transliterated from the test blocks in Ghostty
 * src/terminal/kitty/graphics_unicode.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. All 16
 * upstream `test "..."` blocks are ported; none are omitted. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 *
 * Wisp: departures from a mechanical mapping, all of them in the test
 * harness rather than in the assertions:
 *
 *  1. None in the setup: Terminal.print sets kitty_virtual_placeholder
 *     itself, as upstream does. The port had omitted that block while
 *     kitty_graphics was off; it was restored in
 *     src/vt/terminal_impl.hpp alongside this port. PlacementIterator
 *     skips every row without the flag, so without it these tests would
 *     all see zero placements.
 *
 *  2. The three "unicode render placement" tests use
 *     `kitty_gfx.ImageStorage` (graphics_storage.zig) and
 *     `kitty_gfx.Image` (graphics_image.zig), neither of which is ported
 *     yet. graphics_unicode.hpp takes both as template parameters (see
 *     its header note), so this file supplies the minimum those tests
 *     exercise: an image with a width/height, and
 *     `placeholderTarget(image_id, placement_id)` returning the stored
 *     virtual placement's rows/columns. Nothing else of either type is
 *     reachable from renderPlacement. placeholderTarget's own lookup
 *     semantics are covered by upstream's `storage: placeholderTarget
 *     lookup` test, which belongs to graphics_storage.zig's port.
 *
 *  3. `try t.printString("\u{10EEEE}\u{0305}")` is spelled with the
 *     explicit UTF-8 bytes of each codepoint, as named macros with their
 *     byte counts, so the lengths are counted rather than eyeballed.
 *
 * Mapping:
 *   var t = try Terminal.init(io, alloc, .{...})   TERM(t, cols, rows)
 *   var it = placementIterator(pin, null)          placementIterator(pin, Maybe<Pin>())
 *   it.next().?                                    NEXT(p, it)
 *   try testing.expect(it.next() == null)          NEXT_NULL(it)
 *   try p.renderPlacement(...)                     Error out-param, Error::none
 */

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"

#include "../terminal/kitty/graphics_unicode.hpp"
#include "../vt/terminal.hpp"

using namespace wisp;
using namespace wisp::vt;

namespace gu = ::wisp::terminal::kitty::graphics::graphics_unicode;

typedef PageList::Pin Pin;
typedef page::Cell Cell;
typedef gu::Placement Placement;
typedef gu::PlacementIterator PlacementIterator;
typedef gu::RenderPlacement RenderPlacement;
typedef terminal::sgr::Attribute::Tag A;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: var t = try init(io, alloc, .{...}); defer t.deinit(alloc); */
struct TermHolder {
    Terminal t;
    bool ok;
    zigstd::Allocator alloc;
    explicit TermHolder(const Terminal::Options &o) : alloc(talloc()) { ok = Terminal::init(alloc, o, &t); }
    ~TermHolder() {
        if (ok) t.deinit(alloc);
    }
};

#define TERM(v, c, r)                                                                              \
    Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                  \
    TermHolder v##_holder(v##_opts);                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    Terminal &v = v##_holder.t

#define NOERR(e) ASSERT_TRUE((e) == PageList::IncreaseCapacityError::none)

/* Wisp: const p = it.next().? */
#define NEXT(v, it)                                                                                \
    Placement v;                                                                                   \
    ASSERT_TRUE((it).next(&v))

/* Wisp: try testing.expect(it.next() == null) */
#define NEXT_NULL(it)                                                                              \
    do {                                                                                           \
        Placement _p;                                                                              \
        ASSERT_TRUE(!(it).next(&_p));                                                              \
    } while (0)

/* Wisp: the UTF-8 encodings of the codepoints upstream writes as `\u{...}`.
 * Each is a separate string literal, so adjacent concatenation cannot run
 * a hex escape into the following byte.
 *
 *   U+10EEEE -> F4 8E BB AE   (4 bytes)
 *   U+0305   -> CC 85         (2 bytes)
 *   U+030D   -> CC 8D         (2 bytes)
 *   U+030E   -> CC 8E         (2 bytes)
 *   U+0310   -> CC 90         (2 bytes)
 */
#define CP_PLACEHOLDER "\xF4\x8E\xBB\xAE"
#define CP_0305 "\xCC\x85"
#define CP_030D "\xCC\x8D"
#define CP_030E "\xCC\x8E"
#define CP_0310 "\xCC\x90"


/* Wisp: see departure (2). The minimum of graphics_image.zig's Image and
 * graphics_storage.zig's ImageStorage that renderPlacement/grid touch. */
struct TestImage {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    TestImage() : id(0), width(0), height(0) {}
    TestImage(uint32_t id_, uint32_t w, uint32_t h) : id(id_), width(w), height(h) {}
};

struct TestImageStorage {
    /* Wisp: ImageStorage.Placement, only the fields grid() reads. */
    struct StoredPlacement {
        bool virtual_;
        uint32_t rows;
        uint32_t columns;
        StoredPlacement() : virtual_(false), rows(0), columns(0) {}
    };

    struct Target {
        uint32_t image_id;
        uint32_t placement_id;
        StoredPlacement placement;
        Target() : image_id(0), placement_id(0), placement() {}
    };

    std::vector<Target> placements;

    void addImage(const TestImage &) {}

    void addPlacement(uint32_t image_id, uint32_t placement_id, const StoredPlacement &p) {
        Target t;
        t.image_id = image_id;
        t.placement_id = placement_id;
        t.placement = p;
        placements.push_back(t);
    }

    /* Wisp: ImageStorage.placeholderTarget. An explicit placement_id is an
     * exact match; a zero placement_id picks the first virtual placement
     * for the image, which is all these tests ever store. */
    Maybe<Target> placeholderTarget(uint32_t image_id, uint32_t placement_id) const {
        if (placement_id > 0) {
            for (size_t i = 0; i < placements.size(); i++) {
                if (placements[i].image_id == image_id &&
                    placements[i].placement_id == placement_id) {
                    return Maybe<Target>(placements[i]);
                }
            }
            return Maybe<Target>::none();
        }

        for (size_t i = 0; i < placements.size(); i++) {
            if (placements[i].image_id != image_id) continue;
            if (!placements[i].placement.virtual_) continue;
            return Maybe<Target>(placements[i]);
        }
        return Maybe<Target>::none();
    }
};

/* test "unicode diacritic sorted" */
TEST(kitty_graphics_unicode, unicode_diacritic_sorted) {
    /* diacritics must be sorted since we use a binary search. */
    bool sorted = true;
    for (size_t i = 1; i < gu::diacritics_len; i++) {
        if (!(gu::diacritics[i - 1] < gu::diacritics[i])) sorted = false;
    }
    ASSERT_TRUE(sorted);
}

/* test "unicode diacritic" */
TEST(kitty_graphics_unicode, unicode_diacritic) {
    /* Some spot checks based on Kitty behavior */
    {
        const Maybe<uint32_t> idx = gu::getIndex(0x483);
        ASSERT_TRUE(idx.has);
        ASSERT_EQ(30, idx.value);
    }
    {
        const Maybe<uint32_t> idx = gu::getIndex(0x1d242);
        ASSERT_TRUE(idx.has);
        ASSERT_EQ(294, idx.value);
    }
}

/* test "unicode placement: none" */
TEST(kitty_graphics_unicode, unicode_placement__none) {
    TERM(t, 5, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Single cell */
    ASSERT_TRUE(t.printString("hello\nworld\n1\n2"));

    /* No placements */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    NEXT_NULL(it);
}

/* test "unicode placement: single row/col" */
TEST(kitty_graphics_unicode, unicode_placement__single_row_col) {
    TERM(t, 5, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Single cell */
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: continuation break" */
TEST(kitty_graphics_unicode, unicode_placement__continuation_break) {
    TERM(t, 10, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Two runs because it jumps cols */
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_030E));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
        ASSERT_EQ(1, p.width);
    }
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(2, p.col);
        ASSERT_EQ(1, p.width);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: continuation with diacritics set" */
TEST(kitty_graphics_unicode, unicode_placement__continuation_with_diacritics_set) {
    TERM(t, 10, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Three cells. They'll continue even though they're explicit */
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_030D));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_030E));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
        ASSERT_EQ(3, p.width);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: continuation with no col" */
TEST(kitty_graphics_unicode, unicode_placement__continuation_with_no_col) {
    TERM(t, 10, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Three cells. They'll continue even though they're explicit */
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
        ASSERT_EQ(3, p.width);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: continuation with no diacritics" */
TEST(kitty_graphics_unicode, unicode_placement__continuation_with_no_diacritics) {
    TERM(t, 10, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Three cells. They'll continue even though they're explicit */
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
        ASSERT_EQ(3, p.width);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: run ending" */
TEST(kitty_graphics_unicode, unicode_placement__run_ending) {
    TERM(t, 10, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Three cells. They'll continue even though they're explicit */
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_030D));
    ASSERT_TRUE(t.printString("ABC"));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
        ASSERT_EQ(2, p.width);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: run starting in the middle" */
TEST(kitty_graphics_unicode, unicode_placement__run_starting_in_the_middle) {
    TERM(t, 10, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Three cells. They'll continue even though they're explicit */
    ASSERT_TRUE(t.printString("ABC"));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_030D));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(0, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
        ASSERT_EQ(2, p.width);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: specifying image id as palette" */
TEST(kitty_graphics_unicode, unicode_placement__specifying_image_id_as_palette) {
    TERM(t, 5, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Single cell */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeIndex(A::fg_256, 42)));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(42, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: specifying image id with high bits" */
TEST(kitty_graphics_unicode, unicode_placement__specifying_image_id_with_high_bits) {
    TERM(t, 5, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Single cell */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeIndex(A::fg_256, 42)));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305 CP_030E));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(33554474, p.image_id);
        ASSERT_EQ(0, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
    }
    NEXT_NULL(it);
}

/* test "unicode placement: specifying placement id as palette" */
TEST(kitty_graphics_unicode, unicode_placement__specifying_placement_id_as_palette) {
    TERM(t, 5, 5);
    t.modes.set(terminal::modes::Mode::grapheme_cluster, true);

    /* Single cell */
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeIndex(A::fg_256, 42)));
    NOERR(t.setAttribute(terminal::sgr::Attribute::makeIndex(A::underline_color_256, 21)));
    ASSERT_TRUE(t.printString(CP_PLACEHOLDER CP_0305 CP_0305));

    /* Get our top left pin */
    const Pin pin = t.screens.active->pages.getTopLeft(point::Tag::viewport);

    /* Should have exactly one placement */
    PlacementIterator it = gu::placementIterator(pin, Maybe<Pin>());
    {
        NEXT(p, it);
        ASSERT_EQ(42, p.image_id);
        ASSERT_EQ(21, p.placement_id);
        ASSERT_EQ(0, p.row);
        ASSERT_EQ(0, p.col);
    }
    NEXT_NULL(it);
}

/* Fish:
 * printf "\033_Gf=100,i=1,t=f,q=2;$(printf dog.png | base64)\033\\"
 * printf "\e[38;5;1m\U10EEEE\U0305\U0305\U10EEEE\U0305\U030D\U10EEEE\U0305\U030E\U10EEEE\U0305\U0310\e[39m\n"
 * printf "\e[38;5;1m\U10EEEE\U030D\U0305\U10EEEE\U030D\U030D\U10EEEE\U030D\U030E\U10EEEE\U030D\U0310\e[39m\n"
 * printf "\033_Ga=p,i=1,U=1,q=2,c=4,r=2\033\\"
 */
/* test "unicode render placement: dog 4x2" */
TEST(kitty_graphics_unicode, unicode_render_placement__dog_4x2) {
    const uint32_t cell_width = 36;
    const uint32_t cell_height = 80;

    TERM(t, 100, 100);
    TestImageStorage s;

    const TestImage image(1, 500, 306);
    s.addImage(image);
    {
        TestImageStorage::StoredPlacement sp;
        sp.virtual_ = true;
        sp.columns = 4;
        sp.rows = 2;
        s.addPlacement(1, 0, sp);
    }

    /* Row 1 */
    {
        Placement p;
        p.pin = *t.screens.active->cursor.page_pin;
        p.image_id = 1;
        p.placement_id = 0;
        p.col = 0;
        p.row = 0;
        p.width = 4;
        p.height = 1;
        RenderPlacement rp;
        ASSERT_TRUE(p.renderPlacement(&s, &image, cell_width, cell_height, &rp) ==
                    Placement::Error::none);
        ASSERT_EQ(0, rp.offset_x);
        ASSERT_EQ(36, rp.offset_y);
        ASSERT_EQ(0, rp.source_x);
        ASSERT_EQ(0, rp.source_y);
        ASSERT_EQ(500, rp.source_width);
        ASSERT_EQ(153, rp.source_height);
        ASSERT_EQ(144, rp.dest_width);
        ASSERT_EQ(44, rp.dest_height);
    }
    /* Row 2 */
    {
        Placement p;
        p.pin = *t.screens.active->cursor.page_pin;
        p.image_id = 1;
        p.placement_id = 0;
        p.col = 0;
        p.row = 1;
        p.width = 4;
        p.height = 1;
        RenderPlacement rp;
        ASSERT_TRUE(p.renderPlacement(&s, &image, cell_width, cell_height, &rp) ==
                    Placement::Error::none);
        ASSERT_EQ(0, rp.offset_x);
        ASSERT_EQ(0, rp.offset_y);
        ASSERT_EQ(0, rp.source_x);
        ASSERT_EQ(153, rp.source_y);
        ASSERT_EQ(500, rp.source_width);
        ASSERT_EQ(153, rp.source_height);
        ASSERT_EQ(144, rp.dest_width);
        ASSERT_EQ(44, rp.dest_height);
    }
}

/* Fish:
 * printf "\033_Gf=100,i=1,t=f,q=2;$(printf dog.png | base64)\033\\"
 * printf "\e[38;5;1m\U10EEEE\U0305\U0305\U10EEEE\U0305\U030D\U10EEEE\U0305\U030E\U10EEEE\U0305\U0310\e[39m\n"
 * printf "\e[38;5;1m\U10EEEE\U030D\U0305\U10EEEE\U030D\U030D\U10EEEE\U030D\U030E\U10EEEE\U030D\U0310\e[39m\n"
 * printf "\033_Ga=p,i=1,U=1,q=2,c=2,r=2\033\\"
 */
/* test "unicode render placement: dog 2x2 with blank cells" */
TEST(kitty_graphics_unicode, unicode_render_placement__dog_2x2_with_blank_cells) {
    const uint32_t cell_width = 36;
    const uint32_t cell_height = 80;

    TERM(t, 100, 100);
    TestImageStorage s;

    const TestImage image(1, 500, 306);
    s.addImage(image);
    {
        TestImageStorage::StoredPlacement sp;
        sp.virtual_ = true;
        sp.columns = 2;
        sp.rows = 2;
        s.addPlacement(1, 0, sp);
    }

    /* Row 1 */
    {
        Placement p;
        p.pin = *t.screens.active->cursor.page_pin;
        p.image_id = 1;
        p.placement_id = 0;
        p.col = 0;
        p.row = 0;
        p.width = 4;
        p.height = 1;
        RenderPlacement rp;
        ASSERT_TRUE(p.renderPlacement(&s, &image, cell_width, cell_height, &rp) ==
                    Placement::Error::none);
        ASSERT_EQ(0, rp.offset_x);
        ASSERT_EQ(58, rp.offset_y);
        ASSERT_EQ(0, rp.source_x);
        ASSERT_EQ(0, rp.source_y);
        ASSERT_EQ(500, rp.source_width);
        ASSERT_EQ(153, rp.source_height);
        ASSERT_EQ(72, rp.dest_width);
        ASSERT_EQ(22, rp.dest_height);
    }
    /* Row 2 */
    {
        Placement p;
        p.pin = *t.screens.active->cursor.page_pin;
        p.image_id = 1;
        p.placement_id = 0;
        p.col = 0;
        p.row = 1;
        p.width = 4;
        p.height = 1;
        RenderPlacement rp;
        ASSERT_TRUE(p.renderPlacement(&s, &image, cell_width, cell_height, &rp) ==
                    Placement::Error::none);
        ASSERT_EQ(0, rp.offset_x);
        ASSERT_EQ(0, rp.offset_y);
        ASSERT_EQ(0, rp.source_x);
        ASSERT_EQ(153, rp.source_y);
        ASSERT_EQ(500, rp.source_width);
        ASSERT_EQ(153, rp.source_height);
        ASSERT_EQ(72, rp.dest_width);
        ASSERT_EQ(22, rp.dest_height);
    }
}

/* Fish:
 * printf "\033_Gf=100,i=1,t=f,q=2;$(printf dog.png | base64)\033\\"
 * printf "\e[38;5;1m\U10EEEE\U0305\U0305\U10EEEE\U0305\U030D\U10EEEE\U0305\U030E\U10EEEE\U0305\U0310\e[39m\n"
 * printf "\033_Ga=p,i=1,U=1,q=2,c=1,r=1\033\\"
 */
/* test "unicode render placement: dog 1x1" */
TEST(kitty_graphics_unicode, unicode_render_placement__dog_1x1) {
    const uint32_t cell_width = 36;
    const uint32_t cell_height = 80;

    TERM(t, 100, 100);
    TestImageStorage s;

    const TestImage image(1, 500, 306);
    s.addImage(image);
    {
        TestImageStorage::StoredPlacement sp;
        sp.virtual_ = true;
        sp.columns = 1;
        sp.rows = 1;
        s.addPlacement(1, 0, sp);
    }

    /* Row 1 */
    {
        Placement p;
        p.pin = *t.screens.active->cursor.page_pin;
        p.image_id = 1;
        p.placement_id = 0;
        p.col = 0;
        p.row = 0;
        p.width = 4;
        p.height = 1;
        RenderPlacement rp;
        ASSERT_TRUE(p.renderPlacement(&s, &image, cell_width, cell_height, &rp) ==
                    Placement::Error::none);
        ASSERT_EQ(0, rp.offset_x);
        ASSERT_EQ(29, rp.offset_y);
        ASSERT_EQ(0, rp.source_x);
        ASSERT_EQ(0, rp.source_y);
        ASSERT_EQ(500, rp.source_width);
        ASSERT_EQ(306, rp.source_height);
        ASSERT_EQ(36, rp.dest_width);
        ASSERT_EQ(22, rp.dest_height);
    }
}

TEST(kitty_graphics_unicode, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
