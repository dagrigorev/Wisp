/* Transliterated from the test blocks in Ghostty's
 * src/terminal/kitty/graphics_storage.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * All 71 upstream tests, same names, same inputs, same assertions, in
 * upstream's order. Where an assertion differs it carries a `Wisp:` note
 * naming the upstream assertion it replaces.
 *
 * Wisp, the test-harness seams:
 *
 *   - `test_helpers.h`'s `TEST(suite, name)` takes a C identifier, not a
 *     string, so each upstream name is spelled with `: ` dropped and
 *     spaces, `/` and `-` as underscores, as in the sibling port test
 *     files. `test "storage: delete by row 1x1"` is
 *     `TEST(kitty_graphics_storage, storage_delete_by_row_1x1)`. No test
 *     is renamed beyond that mechanical transformation.
 *
 *   - `std.Io` is not carried over, so the `io` argument is dropped from
 *     every call (see graphics_storage.hpp).
 *
 *   - Upstream's `var t = try terminal.Terminal.init(io, alloc, .{...});
 *     defer t.deinit(alloc);` is the `TERM`/`TERM_OPTS` macros below, as
 *     in test_kitty_graphics_unicode_port.cpp. `var s: ImageStorage = .{}`
 *     plus `defer s.deinit(alloc, t.screens.active)` is `STORAGE`.
 *
 *   - 15 upstream tests reach the storage through
 *     `&t.screens.active.kitty_images`. Wisp's `Screen` carries no
 *     `kitty_images` member (it is ported in upstream's
 *     `kitty_graphics == false` shape), and connecting `ImageStorage`
 *     into `Screen` is a separate change, so those tests use a locally
 *     owned `ImageStorage` instead. Two consequences, both handled
 *     explicitly and noted at each site:
 *
 *       * `storage: erase display preserves scrollback and reclaims
 *         unplaced images` drives `t.eraseDisplay(.complete, false)`,
 *         which under `kitty_graphics` runs
 *         `screens.active.kitty_images.clearScreen(...)` (Terminal.zig
 *         `eraseDisplay`, the `.complete` arm). The port calls
 *         `eraseDisplay` and then that exact `clearScreen` call. Every
 *         upstream assertion is kept.
 *
 *       * the 11 scroll-margin tests rely on Terminal wrapping IND / RI /
 *         SU / SD in `kittyScrollMarginsBegin` + `ScrollMargins.end()`
 *         (Terminal.zig `indexScrollWithImages`, `scrollUp`, `scrollDown`,
 *         `kittyScrollMarginsBegin`). The `kittyIndex` / `kittyReverseIndex`
 *         / `kittyScrollUp` / `kittyScrollDown` helpers below reproduce
 *         that wrapping verbatim around the real `Terminal` calls, so the
 *         tests exercise the ported `scrollMarginsBegin`/`end` on the same
 *         row operations and with the same `ScrollOp` upstream picks. IL/DL
 *         are deliberately left unwrapped, as upstream leaves them. Every
 *         upstream assertion is kept.
 *
 *         One ordering difference: upstream's `scrollUp`/`scrollDown`
 *         declare the cursor-restore `defer` before the kitty one, so Zig
 *         runs `ScrollMargins.end()` *before* the cursor is restored. The
 *         helpers here call `t.scrollUp(...)` (which restores the cursor
 *         itself) and then `end()`. `end()` only repositions tracked pins
 *         from active-area coordinates and never reads the cursor, so the
 *         result is identical.
 *
 *   - `ImageStorage.delete` is `delete_` (`delete` is a C++ keyword) and
 *     `Placement.Location.virtual` is `virtual_`.
 *
 *   - `try testing.expectError(error.OutOfMemory, expr)` is
 *     `ASSERT_TRUE(!expr)`; the port returns false for OutOfMemory.
 *
 *   - `test_helpers.h`'s `ASSERT_EQ` casts both sides to `long`, which
 *     truncates 64-bit values and pointers, so generations, pointers and
 *     `maxInt(u32)` are compared with `ASSERT_TRUE(a == b)`.
 *
 *   - `testing.allocator`'s leak check is `zigstd::testing_state().live`,
 *     asserted by the trailing `zz_Wisp_no_leaks` test.
 */

#include <new>
#include <stdint.h>
#include <string.h>

#include "test_helpers.h"
#include "../terminal/kitty/graphics_storage.hpp"
#include "../vt/screen_impl.hpp"
#include "../vt/terminal_impl.hpp"
#include "../zigstd/allocator.hpp"

using namespace wisp;
using namespace wisp::vt;

namespace gs = ::wisp::terminal::kitty::graphics_storage;
namespace gi = ::wisp::terminal::kitty::graphics_image;
namespace ganim = ::wisp::terminal::kitty::graphics_animation;
namespace gcmd = ::wisp::terminal::kitty::graphics;

typedef gs::ImageStorage ImageStorage;
typedef ImageStorage::Placement Placement;
typedef ImageStorage::PlacementKey PlacementKey;
typedef ImageStorage::PlacementId PlacementId;
typedef gi::Image Image;
typedef gi::Rect Rect;
typedef PageList::Pin Pin;
typedef gcmd::Delete::Action Action;
typedef ganim::Animation Animation;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

/* Wisp: var t = try terminal.Terminal.init(io, alloc, .{...}); defer t.deinit(alloc); */
struct TermHolder {
    Terminal t;
    bool ok;
    zigstd::Allocator alloc;
    explicit TermHolder(const Terminal::Options &o) : alloc(talloc()) {
        ok = Terminal::init(alloc, o, &t);
    }
    ~TermHolder() {
        if (ok) t.deinit(alloc);
    }
};

#define TERM(v, c, r)                                                                              \
    Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                  \
    TermHolder v##_holder(v##_opts);                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    Terminal &v = v##_holder.t

#define TERM_SB(v, c, r, sb)                                                                       \
    Terminal::Options v##_opts((size::CellCountInt)(c), (size::CellCountInt)(r));                  \
    v##_opts.max_scrollback_bytes = Maybe<size_t>((size_t)(sb));                                   \
    TermHolder v##_holder(v##_opts);                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    Terminal &v = v##_holder.t

/* Wisp: var s: ImageStorage = .{}; defer s.deinit(alloc, t.screens.active); */
struct StorageHolder {
    ImageStorage s;
    zigstd::Allocator alloc;
    Screen *screen;
    StorageHolder(zigstd::Allocator a, Screen *sc) : alloc(a), screen(sc) {}
    ~StorageHolder() { s.deinit(alloc, screen); }
};

#define STORAGE(v, t)                                                                              \
    StorageHolder v##_holder(talloc(), (t).screens.active);                                        \
    ImageStorage &v = v##_holder.s

/* Our pin for the placement */
static Pin *trackPin(Terminal *t, const point::Coordinate &pt) {
    const Maybe<Pin> p = t->screens.active->pages.pin(point::Point(point::Tag::active, pt));
    if (!p.has) return nullptr;
    return t->screens.active->pages.trackPin(p.value);
}

static point::Coordinate xy(size::CellCountInt x, uint32_t y) {
    return point::Coordinate(x, y);
}

/* ── Wisp: `.{ .location = ... }` placement literals. ──────────────── */

static Placement pinAt(Pin *p) {
    Placement r;
    r.location = Placement::Location::makePin(p);
    return r;
}

static Placement virt() {
    Placement r;
    r.location = Placement::Location::makeVirtual();
    return r;
}

static Placement relTo(const PlacementKey &parent, int32_t h = 0, int32_t v = 0) {
    Placement::Relative rel;
    rel.parent = parent;
    rel.horizontal_offset = h;
    rel.vertical_offset = v;
    Placement r;
    r.location = Placement::Location::makeRelative(rel);
    return r;
}

static PlacementKey extKey(uint32_t image_id, uint32_t placement_id) {
    return PlacementKey(image_id, PlacementId::external(placement_id));
}

static PlacementKey intKey(uint32_t image_id, uint32_t placement_id) {
    return PlacementKey(image_id, PlacementId::internal(placement_id));
}

/* ── Wisp: `.{ .id = 1, .width = 50, ... }` image literals. ────────── */

static Image imgId(uint32_t id) {
    Image i;
    i.id = id;
    return i;
}

static Image imgWh(uint32_t id, uint32_t w, uint32_t h) {
    Image i;
    i.id = id;
    i.width = w;
    i.height = h;
    return i;
}

static Image imgWhOnly(uint32_t w, uint32_t h) {
    Image i;
    i.width = w;
    i.height = h;
    return i;
}

/* Wisp: `try alloc.dupe(u8, "*" ** n)`. */
static uint8_t *dupeFill(zigstd::Allocator alloc, uint8_t c, size_t n) {
    uint8_t *p = alloc.alloc(n);
    if (p != nullptr) memset(p, c, n);
    return p;
}

static uint8_t *dupeStr(zigstd::Allocator alloc, const char *s) {
    return alloc.dupe((const uint8_t *)s, strlen(s));
}

/* ── Wisp: `.{ .all = true }` etc. delete-action literals. ─────────── */

static Action actAll(bool v) {
    Action a;
    a.tag = Action::Tag::all;
    a.all = v;
    return a;
}

static Action actId(uint32_t image_id, uint32_t placement_id, bool del) {
    Action a;
    a.tag = Action::Tag::id;
    a.id.image_id = image_id;
    a.id.placement_id = placement_id;
    a.id.delete_ = del;
    return a;
}

static Action actIntersectCursor(bool v) {
    Action a;
    a.tag = Action::Tag::intersect_cursor;
    a.intersect_cursor = v;
    return a;
}

static Action actIntersectCell(uint32_t x, uint32_t y, bool del) {
    Action a;
    a.tag = Action::Tag::intersect_cell;
    a.intersect_cell.x = x;
    a.intersect_cell.y = y;
    a.intersect_cell.delete_ = del;
    return a;
}

static Action actColumn(uint32_t x, bool del) {
    Action a;
    a.tag = Action::Tag::column;
    a.column.x = x;
    a.column.delete_ = del;
    return a;
}

static Action actRow(uint32_t y, bool del) {
    Action a;
    a.tag = Action::Tag::row;
    a.row.y = y;
    a.row.delete_ = del;
    return a;
}

static Action actRange(uint32_t first, uint32_t last, bool del) {
    Action a;
    a.tag = Action::Tag::range;
    a.range.first = first;
    a.range.last = last;
    a.range.delete_ = del;
    return a;
}

/* ── Wisp: Terminal.zig's kitty scroll wrapping, see the header note. ── */

static bool kittyScrollUp(ImageStorage &s, Terminal &t, size_t count);

/* Full-screen scrolls need no adjustment: placements follow their
 * anchored rows (possibly into the scrollback) via pin tracking. */
static bool kittyScrollMarginsBegin(ImageStorage &s, Terminal &t, int64_t delta,
                                    ImageStorage::ScrollOp op,
                                    ImageStorage::ScrollMargins *out) {
    if (t.scrolling_region.top == 0 && t.scrolling_region.bottom == t.rows - 1 &&
        t.scrolling_region.left == 0 && t.scrolling_region.right == t.cols - 1)
        return false;
    *out = s.scrollMarginsBegin(&t, delta, op);
    return true;
}

static bool kittyIndex(ImageStorage &s, Terminal &t) {
    Screen *screen = t.screens.active;

    /* The index() branch that scrolls: cursor inside the region, on its
     * bottom-most row, within the left/right margins. */
    const bool scrolls = screen->cursor.y >= t.scrolling_region.top &&
                         screen->cursor.y == t.scrolling_region.bottom &&
                         screen->cursor.x >= t.scrolling_region.left &&
                         screen->cursor.x <= t.scrolling_region.right;
    if (!scrolls || s.placements.count() == 0) return t.index();

    /* Slow path for left and right scrolling region margins.
     * scrollUp handles the kitty image adjustment itself. */
    if (t.scrolling_region.left != 0 || t.scrolling_region.right != t.cols - 1)
        return kittyScrollUp(s, t, 1);

    const ImageStorage::ScrollOp op =
        (t.scrolling_region.top == 0 &&
         (!screen->no_scrollback || t.scrolling_region.bottom == 0))
            ? ImageStorage::ScrollOp::window_shift
            : ImageStorage::ScrollOp::in_place;

    ImageStorage::ScrollMargins st;
    const bool has = kittyScrollMarginsBegin(s, t, -1, op, &st);
    const bool ok = t.index();
    if (has) st.end();
    return ok;
}

static bool kittyScrollUp(ImageStorage &s, Terminal &t, size_t count) {
    ImageStorage::ScrollMargins st;
    bool has = false;
    if (s.placements.count() != 0) {
        /* The op must mirror the branch below: the scrollback path
         * shifts the active window while the deleteLines path
         * moves rows in place. */
        const size_t region_height =
            (size_t)(t.scrolling_region.bottom - t.scrolling_region.top) + 1;
        const size_t n = count < region_height ? count : region_height;
        const ImageStorage::ScrollOp op =
            (t.scrolling_region.top == 0 && t.scrolling_region.left == 0 &&
             t.scrolling_region.right == t.cols - 1 &&
             (!t.screens.active->no_scrollback ||
              t.scrolling_region.bottom == t.rows - 1))
                ? ImageStorage::ScrollOp::window_shift
                : ImageStorage::ScrollOp::in_place;
        has = kittyScrollMarginsBegin(s, t, -(int64_t)n, op, &st);
    }
    const bool ok = t.scrollUp(count);
    if (has) st.end();
    return ok;
}

static void kittyScrollDown(ImageStorage &s, Terminal &t, size_t count) {
    ImageStorage::ScrollMargins st;
    bool has = false;
    if (s.placements.count() != 0) {
        const size_t region_height =
            (size_t)(t.scrolling_region.bottom - t.scrolling_region.top) + 1;
        const size_t n = count < region_height ? count : region_height;
        has = kittyScrollMarginsBegin(s, t, (int64_t)n, ImageStorage::ScrollOp::in_place, &st);
    }
    t.scrollDown(count);
    if (has) st.end();
}

static void kittyReverseIndex(ImageStorage &s, Terminal &t) {
    Screen *screen = t.screens.active;
    if (screen->cursor.y != t.scrolling_region.top ||
        screen->cursor.x < t.scrolling_region.left ||
        screen->cursor.x > t.scrolling_region.right) {
        t.reverseIndex();
        return;
    }

    kittyScrollDown(s, t, 1);
}

/* ── Tests ────────────────────────────────────────────────────────── */

TEST(kitty_graphics_storage, storage_add_placement_with_zero_placement_id) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 50, 50)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 25, 25)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(25, 25)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(25, 25)))));

    ASSERT_EQ(2, s.placements.count());
    ASSERT_EQ(2, s.images.count());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(intKey(1, 0)));
    ASSERT_TRUE(s.placements.contains(intKey(1, 1)));
}

TEST(kitty_graphics_storage, storage_replacing_placement_releases_tracked_pin) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));

    const size_t tracked = t.screens.active->pages.countTrackedPins();
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());

    Pin *replacement_pin = trackPin(&t, xy(2, 2));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(replacement_pin)));
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());
    Placement got;
    ASSERT_TRUE(s.placements.get(extKey(1, 1), &got));
    ASSERT_TRUE(replacement_pin == got.location.pin);
}

TEST(kitty_graphics_storage, storage_adding_placement_reclaims_garbage_placements) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));

    const size_t tracked = t.screens.active->pages.countTrackedPins();
    Pin *old_pin = trackPin(&t, xy(0, 0));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(old_pin)));
    old_pin->garbage = true;

    Pin *new_pin = trackPin(&t, xy(1, 1));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(new_pin)));

    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());
    Placement got;
    ASSERT_TRUE(s.placements.get(intKey(1, 1), &got));
    ASSERT_TRUE(new_pin == got.location.pin);
}

TEST(kitty_graphics_storage, storage_placement_count_limit_permits_replacement) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, virt()));

    Image *img = s.images.getPtr(1);
    ASSERT_NOT_NULL(img);
    /* Wisp: `std.math.maxInt(@TypeOf(img.metadata.placement_count))`, i.e.
     * maxInt(u30). graphics_image.hpp carries the u30 in a uint32_t, so
     * graphics_storage.hpp spells the limit as `placement_count_max`. */
    img->metadata.placement_count = gs::placement_count_max;
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, virt()));
    ASSERT_TRUE(!s.addPlacement(alloc, t.screens.active, 1, 2, virt()));
    ASSERT_EQ(1, s.placements.count());
}

TEST(kitty_graphics_storage, storage_delete_all_visible_placements_and_matching_images) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actAll(true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.images.count());
    ASSERT_TRUE(s.images.contains(3));
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_all_placements_and_images_preserves_limit) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    s.total_limit = 5000;
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actAll(true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.images.count());
    ASSERT_TRUE(s.images.contains(3));
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(5000, s.total_limit);
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_all_visible_placements_preserves_scrollback) {
    const zigstd::Allocator alloc = talloc();
    TERM_SB(t, 2, 2, 4096);
    t.width_px = 2;
    t.height_px = 2;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 1, 1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(t.scrollUp(1));

    const PlacementKey history_key = extKey(1, 1);
    Placement hist;
    ASSERT_TRUE(s.placements.get(history_key, &hist));
    ASSERT_TRUE(!t.screens.active->pages
                     .pointFromPin(point::Tag::active, *hist.location.pin)
                     .has);

    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(0, 0)))));

    s.delete_(alloc, &t, actAll(true));
    ASSERT_EQ(1, s.placements.count());
    ASSERT_TRUE(s.placements.contains(history_key));
    ASSERT_EQ(1, s.images.count());
    ASSERT_TRUE(s.images.contains(1));
}

TEST(kitty_graphics_storage, storage_delete_all_includes_placements_spanning_into_active_area) {
    const zigstd::Allocator alloc = talloc();
    TERM_SB(t, 2, 2, 4096);
    t.width_px = 2;
    t.height_px = 2;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 1, 2)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(t.scrollUp(1));

    Placement placement;
    ASSERT_TRUE(s.placements.get(extKey(1, 1), &placement));
    ASSERT_TRUE(!t.screens.active->pages
                     .pointFromPin(point::Tag::active, *placement.location.pin)
                     .has);
    const Maybe<Image> img1 = s.imageById(1);
    ASSERT_TRUE(img1.has);
    Rect rect;
    ASSERT_TRUE(placement.rect(img1.value, &t, &rect));
    ASSERT_TRUE(
        t.screens.active->pages.pointFromPin(point::Tag::active, rect.bottom_right).has);

    s.delete_(alloc, &t, actAll(true));
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(0, s.images.count());
}

TEST(kitty_graphics_storage, storage_delete_all_placements) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actAll(false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_all_placements_by_image_id) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actId(2, 0, false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_all_placements_by_image_id_and_unused_images) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actId(2, 0, true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_placement_by_specific_id) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actId(1, 2, true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(2, s.placements.count());
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(tracked + 2, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_uppercase_id_delete_preserves_image_when_placement_does_not_match) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));

    s.dirty = false;
    const uint64_t generation = s.generation;
    s.delete_(alloc, &t, actId(1, 7, true));

    ASSERT_TRUE(s.imageById(1).has);
    ASSERT_TRUE(!s.dirty);
    ASSERT_TRUE(generation == s.generation);
}

TEST(kitty_graphics_storage, storage_uppercase_id_delete_frees_image_after_placement_matches) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 9, pinAt(trackPin(&t, xy(1, 1)))));

    s.dirty = false;
    s.delete_(alloc, &t, actId(1, 9, true));

    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(0, s.images.count());
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_uppercase_id_delete_without_placement_frees_unplaced_image) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));

    s.dirty = false;
    s.delete_(alloc, &t, actId(1, 0, true));

    ASSERT_EQ(0, s.images.count());
    ASSERT_TRUE(s.dirty);
}

TEST(kitty_graphics_storage, storage_delete_intersecting_cursor) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 50, 50)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 25, 25)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(25, 25)))));

    t.screens.active->cursorAbsolute(12, 12);

    s.dirty = false;
    s.delete_(alloc, &t, actIntersectCursor(false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(extKey(1, 2)));
}

TEST(kitty_graphics_storage, storage_delete_intersecting_cursor_checks_interior_row_column) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(20, 0)))));

    /* This is inside the right placement and on an interior row shared by
     * both placements, but it is outside the left placement's columns. */
    t.screens.active->cursorAbsolute(21, 5);
    s.delete_(alloc, &t, actIntersectCursor(false));

    ASSERT_EQ(1, s.placements.count());
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
}

TEST(kitty_graphics_storage, storage_delete_intersecting_cell_checks_interior_row_column) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(20, 0)))));

    /* Protocol coordinates are one-based, so this targets grid cell (21, 5). */
    s.delete_(alloc, &t, actIntersectCell(22, 6, false));

    ASSERT_EQ(1, s.placements.count());
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
}

TEST(kitty_graphics_storage, storage_delete_intersecting_cursor_plus_unused) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 50, 50)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 25, 25)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(25, 25)))));

    t.screens.active->cursorAbsolute(12, 12);

    s.dirty = false;
    s.delete_(alloc, &t, actIntersectCursor(true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(extKey(1, 2)));
}

TEST(kitty_graphics_storage, storage_delete_intersecting_cursor_hits_multiple) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 50, 50)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 25, 25)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(25, 25)))));

    t.screens.active->cursorAbsolute(26, 26);

    s.dirty = false;
    s.delete_(alloc, &t, actIntersectCursor(true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(1, s.images.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_by_column) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 50, 50)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 25, 25)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(25, 25)))));

    s.dirty = false;
    s.delete_(alloc, &t, actColumn(60, false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
}

TEST(kitty_graphics_storage, storage_delete_by_column_1x1) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 1, 1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(1, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 3, pinAt(trackPin(&t, xy(2, 0)))));

    s.delete_(alloc, &t, actColumn(2, false));
    ASSERT_EQ(2, s.placements.count());
    ASSERT_EQ(1, s.images.count());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
    ASSERT_TRUE(s.placements.contains(extKey(1, 3)));
}

TEST(kitty_graphics_storage, storage_delete_by_row) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 50, 50)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 25, 25)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(25, 25)))));

    s.dirty = false;
    s.delete_(alloc, &t, actRow(60, false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.placements.count());
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
}

TEST(kitty_graphics_storage, storage_delete_by_row_1x1) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 100, 100);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 1, 1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, pinAt(trackPin(&t, xy(0, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 3, pinAt(trackPin(&t, xy(0, 2)))));

    s.delete_(alloc, &t, actRow(2, false));
    ASSERT_EQ(2, s.placements.count());
    ASSERT_EQ(1, s.images.count());

    /* verify the placement is what we expect */
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
    ASSERT_TRUE(s.placements.contains(extKey(1, 3)));
}

TEST(kitty_graphics_storage, storage_delete_images_by_range_1) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(2, s.placements.count());

    s.dirty = false;
    s.delete_(alloc, &t, actRange(1, 2, false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_images_by_range_2) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(2, s.placements.count());

    s.dirty = false;
    s.delete_(alloc, &t, actRange(1, 2, true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(1, s.images.count());
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_images_by_range_3) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 3, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(3, s.placements.count());

    s.dirty = false;
    s.delete_(alloc, &t, actRange(2, 2, false));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(2, s.placements.count());
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
    ASSERT_TRUE(s.placements.contains(extKey(3, 1)));
    ASSERT_EQ(tracked + 2, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_delete_images_by_range_4) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 3, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(3, s.images.count());
    ASSERT_EQ(3, s.placements.count());

    s.dirty = false;
    s.delete_(alloc, &t, actRange(2, 2, true));
    ASSERT_TRUE(s.dirty);
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(2, s.placements.count());
    ASSERT_TRUE(s.placements.contains(extKey(1, 1)));
    ASSERT_TRUE(s.placements.contains(extKey(3, 1)));
    ASSERT_EQ(tracked + 2, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_uppercase_range_deletes_unplaced_image_data) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));

    /* Zero is the default lower bound when x is omitted. */
    s.delete_(alloc, &t, actRange(0, 2, true));
    ASSERT_EQ(1, s.images.count());
    ASSERT_TRUE(s.images.contains(3));
}

TEST(kitty_graphics_storage, storage_delete_images_by_empty_range) {
    /* Ranges that select nothing are a silent no-op, matching kitty, which
     * performs no validation and filters with `x <= image_id <= y`. */
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    /* Inverted range. */
    s.delete_(alloc, &t, actRange(5, 4, true));
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(2, s.placements.count());

    /* Upper bound omitted, so it defaults to zero. */
    s.delete_(alloc, &t, actRange(5, 0, true));
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(2, s.placements.count());

    /* Both bounds omitted. Image IDs are never zero so this matches nothing. */
    s.delete_(alloc, &t, actRange(0, 0, true));
    ASSERT_EQ(2, s.images.count());
    ASSERT_EQ(2, s.placements.count());

    /* Both placements survive, so both of their pins are still tracked. */
    ASSERT_EQ(tracked + 2, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_erase_display_preserves_scrollback_and_reclaims_unplaced_images) {
    const zigstd::Allocator alloc = talloc();
    TERM_SB(t, 2, 2, 4096);
    t.width_px = 2;
    t.height_px = 2;

    /* Wisp: upstream is `const s = &t.screens.active.kitty_images;`. See the
     * header note; the storage is owned by the test here. */
    STORAGE(s_ref, t);
    ImageStorage *s = &s_ref;
    ASSERT_TRUE(s->addImage(alloc, t.screens.active, imgWh(1, 1, 1)));
    ASSERT_TRUE(s->addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(t.scrollUp(1));

    const PlacementKey history_key = extKey(1, 1);
    Placement hist;
    ASSERT_TRUE(s->placements.get(history_key, &hist));
    ASSERT_TRUE(!t.screens.active->pages
                     .pointFromPin(point::Tag::active, *hist.location.pin)
                     .has);

    ASSERT_TRUE(s->addImage(alloc, t.screens.active, imgWh(2, 1, 1)));
    ASSERT_TRUE(s->addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(0, 0)))));
    ASSERT_TRUE(s->addImage(alloc, t.screens.active, imgWh(3, 1, 1)));

    t.eraseDisplay(terminal::csi::EraseDisplay::complete, false);
    /* Wisp: the `build_options.kitty_graphics` block inside
     * Terminal.eraseDisplay's `.complete` arm, which Wisp's Terminal omits. */
    s->clearScreen(alloc, &t);

    ASSERT_EQ(1, s->placements.count());
    ASSERT_TRUE(s->placements.contains(history_key));
    ASSERT_EQ(1, s->images.count());
    ASSERT_TRUE(s->images.contains(1));
    ASSERT_TRUE(!s->images.contains(2));
    ASSERT_TRUE(!s->images.contains(3));
}

TEST(kitty_graphics_storage, storage_cell_offsets_stay_within_explicit_destination_rectangle) {
    TERM(t, 5, 5);
    t.width_px = 50;  /* 10 px per col */
    t.height_px = 100; /* 20 px per row */

    /* Explicit columns and rows describe the far cell boundaries. Offsets
     * move the near edge inward without moving those far edges. */
    Placement placement = virt();
    placement.x_offset = 3;
    placement.y_offset = 4;
    placement.columns = 2;
    placement.rows = 1;
    const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(4, 3), &t);
    ASSERT_EQ(17, actual.width);
    ASSERT_EQ(16, actual.height);
    ASSERT_EQ(2, placement.gridSize(imgWhOnly(4, 3), &t).cols);
    ASSERT_EQ(1, placement.gridSize(imgWhOnly(4, 3), &t).rows);
}

TEST(kitty_graphics_storage, storage_cell_offsets_clamp_to_cell_bounds) {
    TERM(t, 5, 5);
    t.width_px = 50;
    t.height_px = 100;

    Placement placement = virt();
    placement.x_offset = 0xFFFFFFFFu;
    placement.y_offset = 0xFFFFFFFFu;
    placement.columns = 1;
    placement.rows = 1;
    const Placement::CellOffset offset = placement.cellOffset(&t);
    ASSERT_EQ(9, offset.x);
    ASSERT_EQ(19, offset.y);

    /* Even hostile offsets leave one pixel inside the requested cell. */
    const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(1, 1), &t);
    ASSERT_EQ(1, actual.width);
    ASSERT_EQ(1, actual.height);
}

TEST(kitty_graphics_storage, storage_aspect_ratio_calculation_when_only_columns_or_rows_specified) {
    TERM(t, 100, 100);
    t.width_px = 1000;  /* 10 px per col */
    t.height_px = 2000; /* 20 px per row */

    /* Case 1: Only columns specified */
    {
        const Image image = imgWh(1, 16, 9);
        Placement placement = virt();
        placement.columns = 10;
        placement.rows = 0;

        /* Image is 16x9, set to a width of 10 columns, at 10px per column
         * that's 100px width. 100px * (9 / 16) = 56.25, which should round
         * to a height of 56px. */
        const Placement::PixelSize calc_size = placement.pixelSize(image, &t);
        ASSERT_EQ(100, calc_size.width);
        ASSERT_EQ(56, calc_size.height);
    }

    /* Case 2: Only rows specified */
    {
        const Image image = imgWh(2, 16, 9);
        Placement placement = virt();
        placement.columns = 0;
        placement.rows = 5;

        /* Image is 16x9, set to a height of 5 rows, at 20px per row that's
         * 100px height. 100px * (16 / 9) = 177.77..., which should round to
         * a width of 178px. */
        const Placement::PixelSize calc_size = placement.pixelSize(image, &t);
        ASSERT_EQ(178, calc_size.width);
        ASSERT_EQ(100, calc_size.height);
    }
}

TEST(kitty_graphics_storage, storage_default_source_rectangle_is_intersected_before_sizing) {
    TERM(t, 5, 5);

    Placement placement = virt();
    placement.source_x = 3;
    placement.source_y = 1;
    const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(4, 3), &t);
    ASSERT_EQ(1, actual.width);
    ASSERT_EQ(2, actual.height);
}

TEST(kitty_graphics_storage, storage_explicit_source_rectangle_is_intersected_before_sizing) {
    TERM(t, 10, 10);
    t.width_px = 100;
    t.height_px = 100;

    /* The requested 8x8 rectangle intersects this 10x10 image as 2x3.
     * With a two-column destination, the clipped 2:3 aspect ratio produces
     * a 20x30 pixel destination. */
    Placement placement = virt();
    placement.source_x = 8;
    placement.source_y = 7;
    placement.source_width = 8;
    placement.source_height = 8;
    placement.columns = 2;
    const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(10, 10), &t);
    ASSERT_EQ(20, actual.width);
    ASSERT_EQ(30, actual.height);
}

TEST(kitty_graphics_storage, storage_placement_geometry_handles_untrusted_dimensions) {
    const uint32_t max = 0xFFFFFFFFu;

    TERM(t, 2, 2);
    t.width_px = max;
    t.height_px = max;

    /* Cell dimensions multiplied by protocol-controlled row and column
     * counts saturate instead of panicking or wrapping. */
    {
        Placement placement = virt();
        placement.columns = 3;
        placement.rows = 3;
        const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(1, 1), &t);
        ASSERT_TRUE(max == actual.width);
        ASSERT_TRUE(max == actual.height);
    }

    /* Aspect-ratio scaling also saturates when the derived dimension does
     * not fit in the public u32 geometry type. */
    {
        Placement placement = virt();
        placement.columns = 3;
        placement.source_height = max;
        const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(1, 1), &t);
        ASSERT_TRUE(max == actual.width);
        ASSERT_TRUE(max == actual.height);
    }
    {
        Placement placement = virt();
        placement.rows = 3;
        placement.source_width = max;
        const Placement::PixelSize actual = placement.pixelSize(imgWhOnly(1, 1), &t);
        ASSERT_TRUE(max == actual.width);
        ASSERT_TRUE(max == actual.height);
    }

    /* Pixel offsets are protocol-controlled too. Clamp them to the cell
     * before including them in grid geometry. */
    t.width_px = 2;
    t.height_px = 2;
    {
        Placement placement = virt();
        placement.x_offset = max;
        placement.y_offset = max;
        const Placement::GridSize actual = placement.gridSize(imgWhOnly(1, 1), &t);
        ASSERT_EQ(1, actual.cols);
        ASSERT_EQ(1, actual.rows);
    }

    Pin *pin = trackPin(&t, xy(0, 0));
    ASSERT_NOT_NULL(pin);

    /* Explicit maximum dimensions must clamp the rectangle to the terminal
     * without overflowing its horizontal extent. */
    {
        Placement placement = pinAt(pin);
        placement.columns = max;
        placement.rows = 1;
        Rect rect;
        ASSERT_TRUE(placement.rect(imgWhOnly(1, 1), &t, &rect));
        ASSERT_EQ(1, rect.bottom_right.x);
    }

    /* A garbage pin represents content that has been pruned from retained
     * history. Its fallback location must not make the placement visible. */
    pin->garbage = true;
    {
        Placement placement = pinAt(pin);
        placement.columns = 1;
        placement.rows = 1;
        Rect rect;
        ASSERT_TRUE(!placement.rect(imgWhOnly(1, 1), &t, &rect));
    }
    pin->garbage = false;

    /* Terminals can temporarily have no pixel geometry. A placement whose
     * computed grid is empty has no rectangle, so rect must not subtract one
     * from a zero row count. */
    t.width_px = 0;
    t.height_px = 0;
    {
        Placement placement = pinAt(pin);
        placement.columns = 1;
        const Placement::GridSize actual = placement.gridSize(imgWhOnly(1, 1), &t);
        ASSERT_EQ(0, actual.cols);
        ASSERT_EQ(0, actual.rows);
        Rect rect;
        ASSERT_TRUE(!placement.rect(imgWhOnly(1, 1), &t, &rect));
    }

    /* Wisp: upstream's `defer t.screens.active.pages.untrackPin(pin)`. */
    t.screens.active->pages.untrackPin(pin);
}

TEST(kitty_graphics_storage, storage_generation_stamps_on_image_add_and_replace) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);

    /* Fresh storage has generation zero (never mutated). */
    ASSERT_TRUE((uint64_t)0 == s.generation);

    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 1, 1)));
    const uint64_t gen1 = s.generation;
    ASSERT_TRUE(gen1 > 0);

    const Maybe<Image> img1 = s.imageById(1);
    ASSERT_TRUE(img1.has);
    ASSERT_TRUE(gen1 == img1.value.generation);

    /* A second image gets a strictly greater stamp. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(2, 1, 1)));
    const uint64_t gen2 = s.generation;
    ASSERT_TRUE(gen2 > gen1);
    ASSERT_TRUE(gen2 == s.imageById(2).value.generation);

    /* Retransmitting the same image ID (identical dimensions) gets a
     * fresh stamp: this is what makes same-sized retransmissions
     * detectable by renderers. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgWh(1, 1, 1)));
    const uint64_t gen3 = s.generation;
    ASSERT_TRUE(gen3 > gen2);
    ASSERT_TRUE(gen3 == s.imageById(1).value.generation);

    /* Image 2 kept its stamp. */
    ASSERT_TRUE(gen2 == s.imageById(2).value.generation);
}

TEST(kitty_graphics_storage, storage_generation_bumps_on_placement_and_delete) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    const uint64_t gen_add = s.generation;

    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    const uint64_t gen_place = s.generation;
    ASSERT_TRUE(gen_place > gen_add);

    /* Reads don't change the generation. */
    (void)s.imageById(1);
    (void)s.imageByNumber(1);
    ASSERT_TRUE(gen_place == s.generation);

    s.delete_(alloc, &t, actAll(true));
    ASSERT_TRUE(s.generation > gen_place);
}

TEST(kitty_graphics_storage, storage_generation_bumps_when_setLimit_evicts_or_disables) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);

    uint8_t *data = dupeStr(alloc, "1234");
    ASSERT_NOT_NULL(data);
    Image img = imgWh(1, 1, 1);
    img.data = Image::Data::initComplete(data, 4);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, img));
    const uint64_t gen_add = s.generation;

    /* Lowering the limit evicts the image and must mark a mutation. */
    s.dirty = false;
    s.setLimit(alloc, t.screens.active, 1);
    ASSERT_TRUE(s.dirty);
    ASSERT_TRUE(s.generation > gen_add);
    ASSERT_EQ(0, s.images.count());
    const uint64_t gen_evict = s.generation;

    /* Disabling (limit=0) resets the storage and must mark a mutation. */
    s.dirty = false;
    s.setLimit(alloc, t.screens.active, 0);
    ASSERT_TRUE(s.dirty);
    ASSERT_TRUE(s.generation > gen_evict);
}

TEST(kitty_graphics_storage, storage_imageByNumber_returns_most_recently_transmitted) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);

    /* Two images sharing a number: the newest transmission wins,
     * regardless of insertion order or clock resolution. */
    Image a = imgId(1);
    a.number = 7;
    Image b = imgId(2);
    b.number = 7;
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, a));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, b));
    ASSERT_EQ(2, s.imageByNumber(7).value.id);

    /* Retransmit the first: it becomes the newest. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, a));
    ASSERT_EQ(1, s.imageByNumber(7).value.id);
}

TEST(kitty_graphics_storage, storage_nextGeneration_is_unique_and_monotonic) {
    const uint64_t a = gs::nextGeneration();
    const uint64_t b = gs::nextGeneration();
    ASSERT_TRUE(b > a);
    ASSERT_TRUE(a > 0);
}

TEST(kitty_graphics_storage, storage_no_op_delete_does_not_mark_a_mutation) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);

    /* A delete-all on an empty storage (this runs on every screen
     * clear) must not dirty the state or bump the generation. */
    s.delete_(alloc, &t, actAll(true));
    ASSERT_TRUE(!s.dirty);
    ASSERT_TRUE((uint64_t)0 == s.generation);

    /* Same for a delete that matches nothing. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    const uint64_t gen = s.generation;
    s.dirty = false;
    s.delete_(alloc, &t, actId(42, 0, false));
    ASSERT_TRUE(!s.dirty);
    ASSERT_TRUE(gen == s.generation);

    /* But a delete that removes something does mark a mutation. */
    s.delete_(alloc, &t, actId(1, 0, false));
    ASSERT_TRUE(s.dirty);
    ASSERT_TRUE(s.generation > gen);
}

TEST(kitty_graphics_storage, storage_evicts_images_in_priority_order) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    s.total_limit = 192;

    {
        Image i = imgId(1);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        i.metadata.transient = false;
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    {
        Image i = imgId(2);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        i.metadata.transient = true;
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    {
        Image i = imgId(3);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        i.metadata.transient = true;
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    const uint64_t gen = s.generation;
    const bool result = s.evictImage(alloc, t.screens.active, 96);
    ASSERT_TRUE(s.dirty);
    ASSERT_TRUE(s.generation > gen);
    ASSERT_TRUE(result);
    ASSERT_EQ(1, s.images.count());
    ASSERT_TRUE(!s.images.contains(1));
    ASSERT_TRUE(s.images.contains(2));
    ASSERT_TRUE(!s.images.contains(3));
}

TEST(kitty_graphics_storage, storage_eviction_releases_placement_pins) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    s.total_limit = 128;

    const size_t tracked = t.screens.active->pages.countTrackedPins();
    {
        Image i = imgId(1);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(0, 0)))));
    {
        Image i = imgId(2);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(tracked + 2, t.screens.active->pages.countTrackedPins());

    /* Adding a third image evicts the oldest equally-ranked image and its
     * placement. The newer image's placement and tracked pin remain intact. */
    {
        Image i = imgId(3);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    ASSERT_TRUE(!s.images.contains(1));
    ASSERT_TRUE(s.images.contains(2));
    ASSERT_TRUE(s.images.contains(3));
    ASSERT_EQ(1, s.placements.count());
    ASSERT_TRUE(s.placements.contains(extKey(2, 1)));
    ASSERT_EQ(tracked + 1, t.screens.active->pages.countTrackedPins());
}

TEST(kitty_graphics_storage, storage_pending_image_completes_once_and_preserves_age) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    s.total_limit = 16;

    Image p = imgWh(1, 2, 2);
    p.number = 7;
    p.format = gcmd::Transmission::Format::rgba;
    p.data = Image::Data::initPending(16);
    ImageStorage::PendingImage pending;
    ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, p, &pending));
    ASSERT_EQ(16, s.total_bytes);
    ASSERT_EQ(1, s.imageByNumber(7).value.id);
    ASSERT_TRUE(s.imageById(1).value.data.isPending());

    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_EQ(1, s.placements.count());

    uint8_t *wrong = dupeStr(alloc, "short");
    ASSERT_NOT_NULL(wrong);
    const bool wrong_completed = pending.complete(&s, wrong, 5);
    ASSERT_TRUE(!wrong_completed);

    const uint64_t storage_generation = s.generation;
    s.dirty = false;
    uint8_t *pixels = dupeFill(alloc, '*', 16);
    ASSERT_NOT_NULL(pixels);
    ASSERT_TRUE(pending.complete(&s, pixels, 16));
    ASSERT_TRUE(s.dirty);
    ASSERT_TRUE(s.generation > storage_generation);
    ASSERT_TRUE(pending.generation == s.imageById(1).value.generation);
    ASSERT_EQ(16, s.total_bytes);
    {
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(s.imageById(1).value.data.bytes(&got, &got_len));
        ASSERT_EQ(16, got_len);
        ASSERT_EQ(0, memcmp(pixels, got, 16));
    }

    uint8_t *duplicate = dupeFill(alloc, '!', 16);
    ASSERT_NOT_NULL(duplicate);
    const bool duplicate_completed = pending.complete(&s, duplicate, 16);
    ASSERT_TRUE(!duplicate_completed);

    /* Wisp: upstream's two `defer alloc.free(...)` for the rejected buffers. */
    alloc.free(wrong, 5);
    alloc.free(duplicate, 16);
}

TEST(kitty_graphics_storage, storage_stale_pending_completion_loses_to_delete_replacement_and_eviction) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    s.total_limit = 8;

    Image p1 = imgId(1);
    p1.data = Image::Data::initPending(4);
    ImageStorage::PendingImage deleted;
    ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, p1, &deleted));
    s.delete_(alloc, &t, actId(1, 0, true));
    uint8_t *deleted_data = dupeStr(alloc, "gone");
    ASSERT_NOT_NULL(deleted_data);
    const bool deleted_completed = deleted.complete(&s, deleted_data, 4);
    ASSERT_TRUE(!deleted_completed);

    Image p2 = imgId(2);
    p2.data = Image::Data::initPending(4);
    ImageStorage::PendingImage replaced;
    ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, p2, &replaced));
    {
        Image live = imgId(2);
        uint8_t *d = dupeStr(alloc, "live");
        ASSERT_NOT_NULL(d);
        live.data = Image::Data::initComplete(d, 4);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, live));
    }
    uint8_t *replaced_data = dupeStr(alloc, "late");
    ASSERT_NOT_NULL(replaced_data);
    const bool replaced_completed = replaced.complete(&s, replaced_data, 4);
    ASSERT_TRUE(!replaced_completed);
    {
        const uint8_t *got;
        size_t got_len;
        ASSERT_TRUE(s.imageById(2).value.data.bytes(&got, &got_len));
        ASSERT_EQ(4, got_len);
        ASSERT_EQ(0, memcmp("live", got, 4));
    }

    Image p3 = imgId(3);
    p3.data = Image::Data::initPending(4);
    ImageStorage::PendingImage evicted;
    ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, p3, &evicted));
    {
        Image big = imgId(4);
        uint8_t *d = dupeStr(alloc, "12345678");
        ASSERT_NOT_NULL(d);
        big.data = Image::Data::initComplete(d, 8);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, big));
    }
    ASSERT_TRUE(!s.imageById(3).has);
    uint8_t *evicted_data = dupeStr(alloc, "late");
    ASSERT_NOT_NULL(evicted_data);
    const bool evicted_completed = evicted.complete(&s, evicted_data, 4);
    ASSERT_TRUE(!evicted_completed);

    /* Wisp: upstream's three `defer alloc.free(...)`. */
    alloc.free(deleted_data, 4);
    alloc.free(replaced_data, 4);
    alloc.free(evicted_data, 4);
}

TEST(kitty_graphics_storage, storage_replacement_reuses_pending_reservation_and_removes_placements) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    s.total_limit = 12;
    const size_t tracked = t.screens.active->pages.countTrackedPins();

    Image p = imgWh(1, 2, 1);
    p.format = gcmd::Transmission::Format::rgba;
    p.data = Image::Data::initPending(8);
    ImageStorage::PendingImage pending;
    ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, p, &pending));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    {
        Image keep = imgId(2);
        uint8_t *d = dupeStr(alloc, "keep");
        ASSERT_NOT_NULL(d);
        keep.data = Image::Data::initComplete(d, 4);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, keep));
    }

    {
        Image repl = imgWh(1, 2, 1);
        repl.format = gcmd::Transmission::Format::rgba;
        uint8_t *d = dupeStr(alloc, "12345678");
        ASSERT_NOT_NULL(d);
        repl.data = Image::Data::initComplete(d, 8);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, repl));
    }
    ASSERT_TRUE(s.images.contains(1));
    ASSERT_TRUE(s.images.contains(2));
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(tracked, t.screens.active->pages.countTrackedPins());
    ASSERT_EQ(0, s.imageById(1).value.metadata.placement_count);
    ASSERT_EQ(12, s.total_bytes);

    uint8_t *stale = dupeStr(alloc, "snapshot");
    ASSERT_NOT_NULL(stale);
    const bool stale_completed = pending.complete(&s, stale, 8);
    ASSERT_TRUE(!stale_completed);

    /* Growing the replacement requires eviction, but the replacement ID is
     * excluded. The other image supplies the needed bytes. */
    {
        Image grow = imgId(1);
        uint8_t *d = dupeStr(alloc, "1234567890");
        ASSERT_NOT_NULL(d);
        grow.data = Image::Data::initComplete(d, 10);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, grow));
    }
    ASSERT_TRUE(s.images.contains(1));
    ASSERT_TRUE(!s.images.contains(2));
    ASSERT_EQ(0, s.placements.count());
    ASSERT_EQ(10, s.total_bytes);

    s.delete_(alloc, &t, actId(1, 0, true));
    ASSERT_EQ(0, s.images.count());
    ASSERT_EQ(0, s.placements.count());

    /* Wisp: upstream's `defer alloc.free(stale)`. */
    alloc.free(stale, 8);
}

TEST(kitty_graphics_storage, storage_pending_images_share_exact_eviction_ordering) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);
    s.total_limit = 192;

    {
        Image i = imgId(1);
        i.data = Image::Data::initPending(64);
        i.metadata.transient = false;
        ImageStorage::PendingImage tok;
        ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, i, &tok));
    }
    {
        Image i = imgId(2);
        i.data = Image::Data::initPending(64);
        i.metadata.transient = true;
        ImageStorage::PendingImage tok;
        ASSERT_TRUE(s.addPendingImage(alloc, t.screens.active, i, &tok));
    }
    {
        Image i = imgId(3);
        uint8_t *d = dupeFill(alloc, '*', 64);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 64);
        i.metadata.transient = true;
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, pinAt(trackPin(&t, xy(1, 1)))));

    /* ID 3 is transient and unused, so one exact-size eviction is enough. */
    ASSERT_TRUE(s.evictImage(alloc, t.screens.active, 64));
    ASSERT_TRUE(s.images.contains(1));
    ASSERT_TRUE(s.images.contains(2));
    ASSERT_TRUE(!s.images.contains(3));
    ASSERT_EQ(128, s.total_bytes);
}

TEST(kitty_graphics_storage, storage_nextImageId_number_matches_Kitty_get_free_client_id) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);

    typedef ImageStorage::NextImageIdMode Mode;

    /* Empty storage returns 1, as does Kitty. */
    ASSERT_EQ(1, s.nextImageId(Mode::explicit_));

    /* Contiguous IDs from 1: the next ID after them. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(3)));
    ASSERT_EQ(4, s.nextImageId(Mode::explicit_));

    /* The first gap is filled, even when higher IDs exist. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(100)));
    s.delete_(alloc, &t, actId(2, 0, true));
    ASSERT_EQ(2, s.nextImageId(Mode::explicit_));

    /* 1 is reused as soon as it is free. */
    s.delete_(alloc, &t, actId(1, 0, true));
    ASSERT_EQ(1, s.nextImageId(Mode::explicit_));
}

TEST(kitty_graphics_storage, storage_nextImageId_implicit_skips_in_use_ids_and_zero) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 3, 3);

    STORAGE(s, t);

    typedef ImageStorage::NextImageIdMode Mode;

    /* A client image already sits on the counter's first two values. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2147483647u)));
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2147483648u)));
    ASSERT_TRUE(2147483649u == s.nextImageId(Mode::implicit));
    ASSERT_TRUE(2147483650u == s.nextImageId(Mode::implicit));

    /* Wrapping skips zero, which means "no ID" protocol-wide. */
    s.next_image_id = 0xFFFFFFFFu;
    ASSERT_TRUE(0xFFFFFFFFu == s.nextImageId(Mode::implicit));
    ASSERT_TRUE(1u == s.nextImageId(Mode::implicit));
}

TEST(kitty_graphics_storage, storage_scroll_margins_placement_inside_region_scrolls_and_clips_at_top) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* 2-row image (10x10px cells) inside the region (rows 1-4).
     * Wisp: upstream is `&t.screens.active.kitty_images`. */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 20)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 2)))));
    t.setTopAndBottomMargin(2, 5);
    t.setCursorPos(5, 1);

    /* Entirely inside: moves up with the content. */
    ASSERT_TRUE(kittyIndex(storage, t));
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 1)));
    }

    /* Would extend above the top margin: anchored at the top margin
     * with the top cell row clipped off the source. */
    ASSERT_TRUE(kittyIndex(storage, t));
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 1)));
        ASSERT_EQ(10, p.source_y);
        ASSERT_EQ(10, p.source_height);
    }

    /* Fully clipped: deleted. */
    ASSERT_TRUE(kittyIndex(storage, t));
    ASSERT_EQ(0, storage.placements.count());
    /* The image itself is retained. */
    ASSERT_EQ(1, storage.images.count());
}

TEST(kitty_graphics_storage, storage_scroll_margins_placement_straddling_region_does_not_move) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* 3-row image anchored at the top of a 2-row region: not entirely
     * within the region, so region scrolls must not move it. This
     * exercises the scrollback-creating path (region top at row 0). */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 30)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 0)))));
    t.setTopAndBottomMargin(1, 2);
    t.setCursorPos(2, 1);

    ASSERT_TRUE(kittyIndex(storage, t));
    Placement p;
    ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
    const Maybe<point::Point> pt =
        t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
    ASSERT_TRUE(pt.has);
    ASSERT_TRUE(pt.value.coord().eql(xy(0, 0)));
    ASSERT_EQ(0, p.source_y);
    ASSERT_EQ(0, p.source_height);
}

TEST(kitty_graphics_storage, storage_scroll_margins_placement_below_region_does_not_move) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* A "status bar" image below the bottom margin must stay put while
     * the region above it scrolls (scrollback-creating path). */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 5)))));
    t.setTopAndBottomMargin(1, 4);
    t.setCursorPos(4, 1);

    ASSERT_TRUE(kittyIndex(storage, t));
    ASSERT_TRUE(kittyIndex(storage, t));
    Placement p;
    ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
    const Maybe<point::Point> pt =
        t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
    ASSERT_TRUE(pt.has);
    ASSERT_TRUE(pt.value.coord().eql(xy(0, 5)));
}

TEST(kitty_graphics_storage, storage_scroll_margins_reverse_index_clips_at_bottom) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* 2-row image at the bottom of the region (rows 1-4). */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 20)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 3)))));
    t.setTopAndBottomMargin(2, 5);
    t.setCursorPos(2, 1);

    /* Moves down, with the bottom cell row clipped off the source. */
    kittyReverseIndex(storage, t);
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 4)));
        ASSERT_EQ(0, p.source_y);
        ASSERT_EQ(10, p.source_height);
    }

    /* Fully clipped: deleted. */
    kittyReverseIndex(storage, t);
    ASSERT_EQ(0, storage.placements.count());
}

TEST(kitty_graphics_storage, storage_scroll_margins_scaled_placement_clips_proportionally) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* A 40px-tall image scaled into 2 rows, inside a 2-row region. */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 40)));
    {
        Placement p = pinAt(trackPin(&t, xy(0, 1)));
        p.columns = 1;
        p.rows = 2;
        ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1, p));
    }
    t.setTopAndBottomMargin(2, 3);
    t.setCursorPos(3, 1);

    ASSERT_TRUE(kittyIndex(storage, t));
    Placement p;
    ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
    const Maybe<point::Point> pt =
        t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
    ASSERT_TRUE(pt.has);
    ASSERT_TRUE(pt.value.coord().eql(xy(0, 1)));
    ASSERT_EQ(20, p.source_y);
    ASSERT_EQ(20, p.source_height);
    ASSERT_EQ(1, p.rows);
    ASSERT_EQ(1, p.columns);
}

TEST(kitty_graphics_storage, storage_scroll_margins_left_right_margins_respect_columns) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* One placement outside the left/right margin columns, one inside. */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 2)))));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 2,
                                     pinAt(trackPin(&t, xy(2, 2)))));

    t.modes.set(terminal::modes::Mode::enable_left_and_right_margin, true);
    t.setLeftAndRightMargin(2, 8);

    ASSERT_TRUE(kittyScrollUp(storage, t, 1));

    /* Outside the margin columns: stays. */
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 2)));
    }

    /* Inside: moves up. */
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 2), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(2, 1)));
    }
}

TEST(kitty_graphics_storage, storage_scroll_margins_insert_delete_lines_do_not_move_placements) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* Kitty does not scroll images for IL/DL, only for IND/RI/SU/SD. */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 2)))));
    t.setTopAndBottomMargin(2, 5);
    t.setCursorPos(2, 1);

    t.deleteLines(1);
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 2)));
    }

    t.insertLines(1);
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 2)));
    }
}

TEST(kitty_graphics_storage, storage_scroll_without_margins_moves_placement_into_scrollback) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* No margins: the placement follows its anchored row into the
     * scrollback via pin tracking (kitty's marginless behavior). */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 20)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 0)))));
    t.setCursorPos(6, 1);

    ASSERT_TRUE(kittyIndex(storage, t));
    Placement p;
    ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
    ASSERT_TRUE(!t.screens.active->pages
                     .pointFromPin(point::Tag::active, *p.location.pin)
                     .has);
    const Maybe<point::Point> pt =
        t.screens.active->pages.pointFromPin(point::Tag::screen, *p.location.pin);
    ASSERT_TRUE(pt.has);
    ASSERT_TRUE(pt.value.coord().eql(xy(0, 0)));
}

TEST(kitty_graphics_storage, storage_scroll_margins_large_scroll_deletes_inside_placement) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 2)))));
    t.setTopAndBottomMargin(2, 5);

    ASSERT_TRUE(kittyScrollUp(storage, t, 10));
    ASSERT_EQ(0, storage.placements.count());
}

TEST(kitty_graphics_storage, storage_scroll_margins_straddling_placement_pin_inside_region_restored) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* 2-row image anchored on the bottom margin row, extending below
     * the region: not entirely inside, so it must not move even though
     * the region scroll moves every tracked pin within the region rows. */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 20)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 4)))));
    t.setTopAndBottomMargin(2, 5);
    t.setCursorPos(5, 1);

    ASSERT_TRUE(kittyIndex(storage, t));
    Placement p;
    ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
    const Maybe<point::Point> pt =
        t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
    ASSERT_TRUE(pt.has);
    ASSERT_TRUE(pt.value.coord().eql(xy(0, 4)));
    ASSERT_EQ(0, p.source_y);
    ASSERT_EQ(0, p.source_height);
}

TEST(kitty_graphics_storage, storage_scroll_margins_multi_line_scroll_up_with_scrollback) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 6);
    t.width_px = 100;
    t.height_px = 60;

    /* 1-row image inside a region whose top is the screen top, so the
     * scroll pushes text into the scrollback (window shift) while the
     * image moves within the region. */
    STORAGE(storage, t);
    ASSERT_TRUE(storage.addImage(alloc, t.screens.active, imgWh(1, 10, 10)));
    ASSERT_TRUE(storage.addPlacement(alloc, t.screens.active, 1, 1,
                                     pinAt(trackPin(&t, xy(0, 2)))));
    t.setTopAndBottomMargin(1, 4);

    ASSERT_TRUE(kittyScrollUp(storage, t, 2));
    {
        Placement p;
        ASSERT_TRUE(storage.placements.get(extKey(1, 1), &p));
        const Maybe<point::Point> pt =
            t.screens.active->pages.pointFromPin(point::Tag::active, *p.location.pin);
        ASSERT_TRUE(pt.has);
        ASSERT_TRUE(pt.value.coord().eql(xy(0, 0)));
    }

    /* Another two rows scrolls it out of the region: deleted. */
    ASSERT_TRUE(kittyScrollUp(storage, t, 2));
    ASSERT_EQ(0, storage.placements.count());
}

TEST(kitty_graphics_storage, storage_resolveChain_accumulates_offsets_to_the_pin_root) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 5, 5);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, relTo(extKey(1, 1), 2, 1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 3, relTo(extKey(1, 2), -1, 4)));

    Placement grandchild;
    ASSERT_TRUE(s.placements.get(extKey(1, 3), &grandchild));
    const Maybe<ImageStorage::ResolvedChain> chain =
        s.resolveChain(grandchild.location.relative);
    ASSERT_TRUE(chain.has);
    ASSERT_TRUE(chain.value.root_key.eql(extKey(1, 1)));
    ASSERT_TRUE(chain.value.root.location.tag == Placement::Location::Tag::pin);
    ASSERT_EQ(1, chain.value.horizontal_offset);
    ASSERT_EQ(5, chain.value.vertical_offset);
}

TEST(kitty_graphics_storage, storage_resolveChain_finds_virtual_roots) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 5, 5);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, virt()));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 2, relTo(extKey(1, 1), 1, 0)));

    Placement child;
    ASSERT_TRUE(s.placements.get(extKey(1, 2), &child));
    const Maybe<ImageStorage::ResolvedChain> chain = s.resolveChain(child.location.relative);
    ASSERT_TRUE(chain.has);
    ASSERT_TRUE(chain.value.root.location.tag == Placement::Location::Tag::virtual_);
    ASSERT_TRUE(chain.value.root_key.eql(extKey(1, 1)));
    ASSERT_EQ(1, chain.value.horizontal_offset);
    ASSERT_EQ(0, chain.value.vertical_offset);
}

TEST(kitty_graphics_storage, storage_eviction_removes_orphaned_relative_placements) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 5, 5);

    STORAGE(s, t);
    s.total_limit = 8;

    /* Image 1 holds most of the byte budget and has a pin placement. */
    {
        Image i = imgWh(1, 2, 1);
        uint8_t *d = dupeFill(alloc, 0, 6);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 6);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 1, pinAt(trackPin(&t, xy(1, 1)))));

    /* Image 2's placement is relative to image 1's placement. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(2)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 2, 1, relTo(extKey(1, 1))));

    /* Adding image 3 exceeds the limit and evicts image 1 (oldest),
     * removing its placement, which orphans image 2's placement. */
    {
        Image i = imgWh(3, 2, 1);
        uint8_t *d = dupeFill(alloc, 0, 6);
        ASSERT_NOT_NULL(d);
        i.data = Image::Data::initComplete(d, 6);
        ASSERT_TRUE(s.addImage(alloc, t.screens.active, i));
    }

    ASSERT_TRUE(!s.imageById(1).has);
    ASSERT_EQ(0, s.placements.count());
    ASSERT_TRUE(s.imageById(2).has);
}

TEST(kitty_graphics_storage, storage_placeholderTarget_lookup) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 5, 5);

    STORAGE(s, t);
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, imgId(1)));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 5, virt()));

    /* Exact external ID match. */
    {
        const Maybe<ImageStorage::PlaceholderTarget> target = s.placeholderTarget(1, 5);
        ASSERT_TRUE(target.has);
        const PlacementKey expected = extKey(1, 5);
        ASSERT_TRUE(expected.eql(target.value.key));
        ASSERT_TRUE(target.value.placement.location.tag ==
                    Placement::Location::Tag::virtual_);
    }

    /* Zero placement ID falls back to the image's virtual placement. */
    {
        const Maybe<ImageStorage::PlaceholderTarget> target = s.placeholderTarget(1, 0);
        ASSERT_TRUE(target.has);
        const PlacementKey expected = extKey(1, 5);
        ASSERT_TRUE(expected.eql(target.value.key));
    }

    ASSERT_TRUE(!s.placeholderTarget(1, 9).has);
    ASSERT_TRUE(!s.placeholderTarget(2, 0).has);

    /* With multiple virtual placements, the zero-ID fallback picks
     * deterministically by PlacementId.preferredOver: lowest external. */
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 3, virt()));
    {
        const PlacementKey expected = extKey(1, 3);
        const Maybe<ImageStorage::PlaceholderTarget> target = s.placeholderTarget(1, 0);
        ASSERT_TRUE(target.has);
        ASSERT_TRUE(expected.eql(target.value.key));
    }
}

/* Wisp: `const anim = try alloc.create(animation.Animation); anim.* = .{...}`.
 * zigstd::Allocator hands back raw storage, so the object is placement-new'd. */
static Animation *newAnimation(zigstd::Allocator alloc) {
    Animation *a = alloc.create<Animation>();
    if (a == nullptr) return nullptr;
    new (a) Animation();
    return a;
}

static bool appendFrame(zigstd::Allocator alloc, Animation *anim, uint8_t r, uint8_t g,
                        uint8_t b, uint8_t a_, uint32_t gap_ms) {
    uint8_t *d = alloc.alloc(4);
    if (d == nullptr) return false;
    d[0] = r;
    d[1] = g;
    d[2] = b;
    d[3] = a_;
    Animation::Frame f;
    f.data = d;
    f.data_len = 4;
    f.gap_ms = gap_ms;
    return anim->frames.append(alloc, f);
}

static Image rgbaPixel(zigstd::Allocator alloc, uint32_t id) {
    Image i = imgWh(id, 1, 1);
    i.format = gcmd::Transmission::Format::rgba;
    uint8_t *d = alloc.alloc(4);
    if (d == nullptr) return i;
    d[0] = 255;
    d[1] = 0;
    d[2] = 0;
    d[3] = 255;
    i.data = Image::Data::initComplete(d, 4);
    return i;
}

TEST(kitty_graphics_storage, storage_animation_tick_advances_and_schedules) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 10);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);

    /* A running 1x1 RGBA image whose animation has one extra frame. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, rgbaPixel(alloc, 1)));
    Image *img = s.images.getPtr(1);
    ASSERT_NOT_NULL(img);
    Animation *anim = newAnimation(alloc);
    ASSERT_NOT_NULL(anim);
    anim->state = Animation::State::running;
    img->animation_ = anim;
    ASSERT_TRUE(appendFrame(alloc, anim, 0, 0, 255, 255, 40));

    /* Without a placement the animation doesn't advance (our
     * approximation of Kitty's visibility gate). */
    ASSERT_TRUE(!s.animationTick(0).has);
    ASSERT_EQ(0, anim->current_index);

    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(0, 0)))));
    const uint64_t gen1 = img->generation;
    s.dirty = false;

    /* First tick: the gapless root frame is due immediately and is
     * skipped over to frame 2, which is due again in its 40ms gap. */
    {
        const Maybe<uint64_t> d = s.animationTick(0);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)40 == d.value);
    }
    ASSERT_EQ(1, anim->current_index);
    ASSERT_TRUE(img->generation > gen1);
    ASSERT_TRUE(s.dirty);

    /* Nothing due yet: no advance, and the delay counts down. */
    const uint64_t gen2 = img->generation;
    {
        const Maybe<uint64_t> d = s.animationTick(10);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)30 == d.value);
    }
    ASSERT_TRUE(gen2 == img->generation);

    /* Wrapping is fine with an infinite loop budget: the gapless
     * root is skipped and frame 2 is shown again. */
    {
        const Maybe<uint64_t> d = s.animationTick(40);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)40 == d.value);
    }
    ASSERT_EQ(1, anim->current_index);
    ASSERT_EQ(1, anim->current_loop);
}

TEST(kitty_graphics_storage, storage_animation_tick_loading_state_parks_on_last_frame) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 10);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);

    /* A placed 1x1 RGBA image in the loading state (a=a s=2) whose
     * animation has one extra frame. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, rgbaPixel(alloc, 1)));
    Animation *anim = newAnimation(alloc);
    ASSERT_NOT_NULL(anim);
    anim->state = Animation::State::loading;
    s.images.getPtr(1)->animation_ = anim;
    ASSERT_TRUE(appendFrame(alloc, anim, 0, 0, 255, 255, 40));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(0, 0)))));

    /* Reach the last frame, then park: no wakeup is scheduled while
     * waiting for more frames and the loop counter stays untouched. */
    {
        const Maybe<uint64_t> d = s.animationTick(0);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)40 == d.value);
    }
    ASSERT_EQ(1, anim->current_index);
    ASSERT_TRUE(!s.animationTick(100).has);
    ASSERT_EQ(1, anim->current_index);
    ASSERT_EQ(0, anim->current_loop);

    /* A new frame arriving un-parks playback. */
    ASSERT_TRUE(appendFrame(alloc, anim, 0, 255, 0, 255, 25));
    {
        const Maybe<uint64_t> d = s.animationTick(150);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)25 == d.value);
    }
    ASSERT_EQ(2, anim->current_index);
}

TEST(kitty_graphics_storage, storage_animation_tick_exhausts_loop_budget) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 10);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);

    /* A placed, running 1x1 RGBA image with a gapped root frame, one
     * extra frame, and a one-loop budget (a=a v=2). */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, rgbaPixel(alloc, 1)));
    Animation *anim = newAnimation(alloc);
    ASSERT_NOT_NULL(anim);
    anim->state = Animation::State::running;
    anim->root_gap_ms = 10;
    anim->max_loops = 1;
    s.images.getPtr(1)->animation_ = anim;
    ASSERT_TRUE(appendFrame(alloc, anim, 0, 0, 255, 255, 40));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(0, 0)))));

    /* Root shows for 10ms, frame 2 for 40ms, then the wrap exhausts
     * the budget and playback freezes on the last frame for good. */
    {
        const Maybe<uint64_t> d = s.animationTick(0);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)10 == d.value);
    }
    ASSERT_EQ(0, anim->current_index);
    {
        const Maybe<uint64_t> d = s.animationTick(10);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)40 == d.value);
    }
    ASSERT_EQ(1, anim->current_index);
    ASSERT_TRUE(!s.animationTick(50).has);
    ASSERT_EQ(1, anim->current_index);
    ASSERT_TRUE(!s.animationTick(500).has);
}

TEST(kitty_graphics_storage, storage_animation_tick_ignores_ineligible_animations) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 10);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);

    /* A placed 1x1 RGBA image with one extra frame, in the default
     * stopped state. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, rgbaPixel(alloc, 1)));
    Animation *anim = newAnimation(alloc);
    ASSERT_NOT_NULL(anim);
    s.images.getPtr(1)->animation_ = anim;
    ASSERT_TRUE(appendFrame(alloc, anim, 0, 0, 255, 255, 40));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(0, 0)))));

    /* Stopped (the default) never advances. */
    ASSERT_TRUE(!s.animationTick(0).has);

    /* An all-gapless animation can never advance either. */
    anim->state = Animation::State::running;
    anim->frames.items[0].gap_ms = 0;
    ASSERT_TRUE(!s.animationTick(0).has);
    ASSERT_EQ(0, anim->current_index);
}

TEST(kitty_graphics_storage, storage_animation_tick_re_anchors_a_restarted_clock) {
    const zigstd::Allocator alloc = talloc();
    TERM(t, 10, 10);
    t.width_px = 100;
    t.height_px = 100;

    STORAGE(s, t);

    /* A placed, running 1x1 RGBA image displaying its extra frame,
     * with a shown-at timestamp far ahead of the tick clock. */
    ASSERT_TRUE(s.addImage(alloc, t.screens.active, rgbaPixel(alloc, 1)));
    Animation *anim = newAnimation(alloc);
    ASSERT_NOT_NULL(anim);
    anim->state = Animation::State::running;
    anim->current_index = 1;
    anim->has_frame_shown_at_ms = true;
    anim->frame_shown_at_ms = 1000;
    s.images.getPtr(1)->animation_ = anim;
    ASSERT_TRUE(appendFrame(alloc, anim, 0, 0, 255, 255, 40));
    ASSERT_TRUE(s.addPlacement(alloc, t.screens.active, 1, 0, pinAt(trackPin(&t, xy(0, 0)))));

    /* A timestamp in the future relative to now means the caller's
     * clock restarted; the animation must not stall until the old
     * timestamp comes around again. */
    {
        const Maybe<uint64_t> d = s.animationTick(5);
        ASSERT_TRUE(d.has);
        ASSERT_TRUE((uint64_t)40 == d.value);
    }
    ASSERT_TRUE(anim->has_frame_shown_at_ms);
    ASSERT_TRUE((uint64_t)5 == anim->frame_shown_at_ms);
}

TEST(kitty_graphics_storage, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
