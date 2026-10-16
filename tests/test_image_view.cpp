/* Tests for the image half of the renderer's snapshot: what
 * `VtPane::sync` publishes into `ScreenBuffer.images` from the terminal's
 * Kitty `ImageStorage`.
 *
 * Wisp-specific; there is no upstream counterpart. Ghostty's equivalent
 * tests live in renderer/image.zig and assert on `State.kitty_placements`
 * after a `kittyUpdate`, which is the same seam this file asserts on.
 *
 * What this file CANNOT test: the drawing itself. Everything past the
 * snapshot is Direct2D — `ID2D1Bitmap` uploads, `DrawBitmap`, the clip,
 * the z passes and the device-loss path — and none of it can run without
 * a window and a GPU, so none of it is covered here. The snapshot is the
 * last layer that is pure data, and it is the whole of what is asserted.
 *
 * Images reach the pane the way a client sends them: Kitty APC escapes fed
 * through the pane's stream. Nothing here calls `graphics_exec::execute` or
 * pokes `ImageStorage` directly, so the geometry asserted below is the
 * geometry a real client would get.
 *
 * Wisp, the test-harness seams, as in the sibling test files:
 *   - `test_helpers.h`'s `TEST(suite, name)` takes a C identifier.
 *   - `test_helpers.h`'s `ASSERT_EQ` casts both sides to `long`, which
 *     truncates 64-bit values and pointers, so those are compared with
 *     `ASSERT_TRUE(a == b)`.
 *   - Escapes are written `"\x1b" "_G"` because C++ hex escapes are greedy.
 *   - `testing.allocator`'s leak check is `zigstd::testing_state().live`,
 *     asserted by the trailing `zz_Wisp_no_leaks` test. VtPane allocates
 *     from the C allocator, so this asserts that nothing in the test
 *     itself leaked through the testing allocator.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/vt_pane.hpp"

using namespace wisp;

typedef wisp::terminal::VtPane VtPane;

/* A cell of 10x20 pixels. Chosen so a cell is not square and a mistake
 * that swaps width for height cannot pass. */
static const unsigned kCellW = 10;
static const unsigned kCellH = 20;

struct PaneHolder {
    VtPane p;
    bool ok;
    PaneHolder(unsigned cols, unsigned rows, size_t scrollback_bytes = 65536,
               bool with_cell_size = true) {
        ok = p.init(cols, rows, scrollback_bytes);
        if (ok && with_cell_size) p.setCellSizePx(kCellW, kCellH);
    }
    ~PaneHolder() {
        if (ok) p.deinit();
    }
};

#define PANE(v, c, r)                                                                              \
    PaneHolder v##_holder((c), (r));                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    VtPane &v = v##_holder.p

/* Feed a NUL-terminated literal. Byte lengths with escapes in them are
 * easy to miscount -- two of them in this file were wrong before the
 * lengths were taken out of the tests' hands -- and none of these
 * literals can contain an embedded NUL. */
static void feed(VtPane &p, const char *s) { p.feed(s, strlen(s)); }

/* ── Building a transmission ─────────────────────────────────────────── */

static std::string base64(const unsigned char *data, size_t len) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
    }
    if (i < len) {
        const size_t rem = len - i;
        uint32_t v = (uint32_t)data[i] << 16;
        if (rem == 2) v |= (uint32_t)data[i + 1] << 8;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(rem == 2 ? tbl[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

static std::string decimal(unsigned v) {
    char buf[16];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    std::string s;
    while (n > 0) s.push_back(buf[--n]);
    return s;
}

/* Transmit-and-place (a=T) an RGBA image of the given size. `extra` holds
 * any further Kitty keys, e.g. "z=-1" or "x=5,y=5,w=4,h=4". The pixels are
 * a recognizable ramp; nothing here asserts on them, but a zero-filled
 * buffer would hide a length mistake. */
static void transmitAndPlace(VtPane &p, unsigned id, unsigned w, unsigned h,
                             const char *extra = "") {
    const size_t len = (size_t)w * (size_t)h * 4;
    unsigned char *px = new unsigned char[len];
    for (size_t i = 0; i < len; i++) px[i] = (unsigned char)(i & 0xFF);

    std::string cmd = "\x1b" "_Ga=T,f=32,s=";
    cmd += decimal(w);
    cmd += ",v=";
    cmd += decimal(h);
    cmd += ",i=";
    cmd += decimal(id);
    if (extra[0] != '\0') {
        cmd += ",";
        cmd += extra;
    }
    cmd += ";";
    cmd += base64(px, len);
    cmd += "\x1b" "\\";
    delete[] px;

    p.feed(cmd.c_str(), cmd.size());
}

/* The published placement for an image ID, or NULL. */
static const ScreenImagePlacement *placementOf(const ScreenBuffer &view, uint32_t id) {
    for (int i = 0; i < view.images_len; i++)
        if (view.images[i].image_id == id) return &view.images[i];
    return NULL;
}

/* ── Nothing to publish ──────────────────────────────────────────────── */

TEST(image_view, empty_storage_publishes_nothing) {
    PANE(p, 20, 5);

    feed(p, "hello");
    ASSERT_EQ(0, p.view.images_len);
    ASSERT_EQ(0, p.view.image_ids_len);
    ASSERT_EQ(0, p.view.images_bg_end);
    ASSERT_EQ(0, p.view.images_text_end);
}

TEST(image_view, transmit_without_placement_publishes_no_placement) {
    PANE(p, 20, 5);

    /* a=t transmits only. The image is in storage -- and so must be in
     * the live-ID list, or the renderer would free a bitmap it is about to
     * need -- but there is nothing placed. */
    feed(p, "\x1b" "_Ga=t,f=32,s=1,v=1,i=5;AQIDBA==\x1b" "\\");
    ASSERT_EQ(0, p.view.images_len);
    ASSERT_EQ(1, p.view.image_ids_len);
    ASSERT_TRUE(5u == p.view.image_ids[0]);
    ASSERT_TRUE(screen_view_has_image(&p.view, 5));
    ASSERT_TRUE(!screen_view_has_image(&p.view, 6));
}

/* ── Fully inside the viewport ───────────────────────────────────────── */

TEST(image_view, placement_inside_viewport_is_published) {
    PANE(p, 20, 10);

    /* 20x40 px at a 10x20 cell is exactly 2 cols by 2 rows. */
    transmitAndPlace(p, 7, 20, 40);

    ASSERT_EQ(1, p.view.images_len);
    const ScreenImagePlacement *pl = placementOf(p.view, 7);
    ASSERT_TRUE(pl != NULL);

    ASSERT_TRUE(7u == pl->image_id);
    ASSERT_TRUE(pl->generation != 0);
    ASSERT_TRUE(pl->pixels != NULL);
    ASSERT_EQ(20, (int)pl->pixel_width);
    ASSERT_EQ(40, (int)pl->pixel_height);

    /* Placed at the cursor, which has not moved from the home position. */
    ASSERT_EQ(0, (int)pl->dest_col);
    ASSERT_EQ(0, (int)pl->dest_row);
    ASSERT_EQ(0, (int)pl->cell_offset_x);
    ASSERT_EQ(0, (int)pl->cell_offset_y);

    /* Native size: no c=/r= scaling, so the drawn size is the source. */
    ASSERT_EQ(20, (int)pl->dest_width_px);
    ASSERT_EQ(40, (int)pl->dest_height_px);

    /* No x=/y=/w=/h=, so the source rect is the whole image. */
    ASSERT_EQ(0, (int)pl->source_x);
    ASSERT_EQ(0, (int)pl->source_y);
    ASSERT_EQ(20, (int)pl->source_width);
    ASSERT_EQ(40, (int)pl->source_height);

    ASSERT_EQ(0, (int)pl->z);

    /* z == 0 is not negative, so it belongs to the after-text range. */
    ASSERT_EQ(0, p.view.images_text_end);
}

TEST(image_view, rgb_transmission_is_published_as_rgba) {
    PANE(p, 20, 10);

    /* f=24 is three bytes per pixel; the snapshot only ever carries
     * RGBA8888, so sync must have converted it. A 2x1 RGB image is six
     * bytes, which becomes eight. */
    feed(p, "\x1b" "_Ga=T,f=24,s=2,v=1,i=11;AQIDBAUG\x1b" "\\");

    const ScreenImagePlacement *pl = placementOf(p.view, 11);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(2, (int)pl->pixel_width);
    ASSERT_EQ(1, (int)pl->pixel_height);
    /* Four bytes per pixel are readable, which is what the renderer
     * assumes when it swizzles. */
    ASSERT_TRUE(pl->pixels != NULL);
    ASSERT_EQ(2, (int)pl->source_width);
    ASSERT_EQ(1, (int)pl->source_height);
}

TEST(image_view, cell_size_scaling_uses_columns_and_rows) {
    PANE(p, 20, 10);

    /* c=4,r=2 asks for 4 cols by 2 rows, i.e. 40x40 px at a 10x20 cell,
     * regardless of the image's own 20x40. */
    transmitAndPlace(p, 13, 20, 40, "c=4,r=2");

    const ScreenImagePlacement *pl = placementOf(p.view, 13);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(40, (int)pl->dest_width_px);
    ASSERT_EQ(40, (int)pl->dest_height_px);
    /* The source is still the whole image; the renderer scales. */
    ASSERT_EQ(20, (int)pl->source_width);
    ASSERT_EQ(40, (int)pl->source_height);
}

/* ── Source sub-rectangles ───────────────────────────────────────────── */

TEST(image_view, source_subrect_is_published) {
    PANE(p, 20, 10);

    transmitAndPlace(p, 21, 20, 40, "x=5,y=6,w=4,h=8");

    const ScreenImagePlacement *pl = placementOf(p.view, 21);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(5, (int)pl->source_x);
    ASSERT_EQ(6, (int)pl->source_y);
    ASSERT_EQ(4, (int)pl->source_width);
    ASSERT_EQ(8, (int)pl->source_height);
    /* The full image is still published; the renderer reads the sub-rect
     * out of the one bitmap rather than uploading a cropped copy. */
    ASSERT_EQ(20, (int)pl->pixel_width);
    ASSERT_EQ(40, (int)pl->pixel_height);
    /* Native size follows the source rect, not the image. */
    ASSERT_EQ(4, (int)pl->dest_width_px);
    ASSERT_EQ(8, (int)pl->dest_height_px);
}

TEST(image_view, source_subrect_is_clamped_to_the_image) {
    PANE(p, 20, 10);

    /* A source rect larger than the image is intersected with it, so the
     * renderer never asks Direct2D for pixels outside the bitmap. */
    transmitAndPlace(p, 22, 20, 40, "x=16,y=36,w=1000,h=1000");

    const ScreenImagePlacement *pl = placementOf(p.view, 22);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(16, (int)pl->source_x);
    ASSERT_EQ(36, (int)pl->source_y);
    ASSERT_EQ(4, (int)pl->source_width);
    ASSERT_EQ(4, (int)pl->source_height);
    ASSERT_TRUE(pl->source_x + pl->source_width <= pl->pixel_width);
    ASSERT_TRUE(pl->source_y + pl->source_height <= pl->pixel_height);
}

/* ── Partly outside the viewport ─────────────────────────────────────── */

TEST(image_view, placement_partly_below_viewport_is_clipped_not_dropped) {
    PANE(p, 20, 4);

    /* Put the cursor on the last row and place an image three rows tall:
     * one row is visible and two hang below the viewport. C=1 keeps the
     * cursor still, which also keeps the screen from scrolling the
     * placement back into full view. */
    feed(p, "\x1b" "[4;1H");
    transmitAndPlace(p, 31, 20, 60, "C=1");

    const ScreenImagePlacement *pl = placementOf(p.view, 31);
    ASSERT_TRUE(pl != NULL);
    /* Published with its real position and full size. The renderer clips;
     * the snapshot does not pre-clip, so the visible part keeps its
     * correct scale. */
    ASSERT_EQ(3, (int)pl->dest_row);
    ASSERT_EQ(60, (int)pl->dest_height_px);
    /* The bottom two rows are off the grid -- that is the whole point. */
    ASSERT_TRUE(pl->dest_row + 3 > p.view.rows);
}

TEST(image_view, placement_partly_right_of_viewport_is_clipped_not_dropped) {
    PANE(p, 6, 5);

    /* Column 5 (0-based) of a 6-column pane, with a 4-column image. */
    feed(p, "\x1b" "[1;6H");
    transmitAndPlace(p, 32, 40, 20);

    const ScreenImagePlacement *pl = placementOf(p.view, 32);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(5, (int)pl->dest_col);
    ASSERT_EQ(40, (int)pl->dest_width_px);
}

TEST(image_view, placement_partly_above_viewport_keeps_a_negative_row) {
    PANE(p, 20, 3);

    /* Three rows tall at the top of the screen, then scrolled up by two:
     * only its last row is visible. A pin above the viewport is exactly
     * the case `pointFromPin(.viewport, ...)` cannot express, so the row
     * must come out negative rather than as "not found". */
    transmitAndPlace(p, 33, 20, 60, "C=1");
    ASSERT_EQ(0, (int)p.view.images[0].dest_row);

    feed(p, "\x1b" "[3;1H");
    feed(p, "\r\n\r\n");

    const ScreenImagePlacement *pl = placementOf(p.view, 33);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(-2, (int)pl->dest_row);
    /* Full size still, so the renderer's clip shows the right slice. */
    ASSERT_EQ(60, (int)pl->dest_height_px);
    ASSERT_EQ(0, (int)pl->source_y);
}

/* ── Entirely outside the viewport ───────────────────────────────────── */

TEST(image_view, placement_scrolled_out_of_view_is_not_published) {
    PANE(p, 20, 5);

    transmitAndPlace(p, 41, 20, 40);
    ASSERT_TRUE(placementOf(p.view, 41) != NULL);

    /* Push it far above the viewport. The image is still in storage -- so
     * its bitmap must be kept -- but it has no visible rectangle. */
    for (int i = 0; i < 20; i++) feed(p, "x\r\n");

    ASSERT_TRUE(placementOf(p.view, 41) == NULL);
    ASSERT_EQ(0, p.view.images_len);
}

TEST(image_view, placement_scrolled_back_into_view_is_published_again) {
    PANE(p, 20, 5);

    transmitAndPlace(p, 42, 20, 40);
    for (int i = 0; i < 20; i++) feed(p, "x\r\n");
    ASSERT_TRUE(placementOf(p.view, 42) == NULL);

    /* Scrolling the viewport back up must bring it back: placements hold
     * tracked pins, so they move with the content rather than with the
     * screen. */
    p.scrollViewport(20);
    const ScreenImagePlacement *pl = placementOf(p.view, 42);
    ASSERT_TRUE(pl != NULL);
    ASSERT_TRUE(pl->dest_row >= 0);
    ASSERT_TRUE(pl->dest_row < p.view.rows);

    p.resetViewport();
    ASSERT_TRUE(placementOf(p.view, 42) == NULL);
}

TEST(image_view, image_deleted_from_storage_leaves_the_live_id_list) {
    PANE(p, 20, 5);

    transmitAndPlace(p, 43, 20, 40);
    ASSERT_TRUE(screen_view_has_image(&p.view, 43));

    /* a=d,d=I,i=43 deletes the image and its placements. */
    feed(p, "\x1b" "_Ga=d,d=I,i=43\x1b" "\\");

    ASSERT_TRUE(!screen_view_has_image(&p.view, 43));
    ASSERT_TRUE(placementOf(p.view, 43) == NULL);
}

/* ── Z order ─────────────────────────────────────────────────────────── */

TEST(image_view, placements_are_sorted_by_z_with_the_text_split_at_zero) {
    PANE(p, 20, 10);

    /* Transmitted in an order that is not z order, and at different rows
     * so neither hides the other. */
    feed(p, "\x1b" "[1;1H");
    transmitAndPlace(p, 51, 20, 20, "z=5");
    feed(p, "\x1b" "[3;1H");
    transmitAndPlace(p, 52, 20, 20, "z=-3");
    feed(p, "\x1b" "[5;1H");
    transmitAndPlace(p, 53, 20, 20, "z=0");
    feed(p, "\x1b" "[7;1H");
    transmitAndPlace(p, 54, 20, 20, "z=-1");

    ASSERT_EQ(4, p.view.images_len);

    /* Ascending z. */
    ASSERT_EQ(-3, (int)p.view.images[0].z);
    ASSERT_EQ(-1, (int)p.view.images[1].z);
    ASSERT_EQ(0, (int)p.view.images[2].z);
    ASSERT_EQ(5, (int)p.view.images[3].z);

    /* The glyph pass goes at images_text_end: the two negative-z
     * placements draw behind the text, the two others in front. */
    ASSERT_EQ(2, p.view.images_text_end);

    /* No placement is in the below-the-cell-backgrounds tier, so that
     * range is empty and the behind-text range starts at 0. */
    ASSERT_EQ(0, p.view.images_bg_end);
}

TEST(image_view, equal_z_is_ordered_by_image_id) {
    PANE(p, 20, 10);

    /* The storage's placement map iterates in an unspecified order, so
     * without an explicit tiebreak two images at the same z could swap
     * draw order between frames. Higher IDs draw later (on top). */
    feed(p, "\x1b" "[1;1H");
    transmitAndPlace(p, 77, 20, 20, "z=2");
    feed(p, "\x1b" "[3;1H");
    transmitAndPlace(p, 66, 20, 20, "z=2");
    feed(p, "\x1b" "[5;1H");
    transmitAndPlace(p, 88, 20, 20, "z=2");

    ASSERT_EQ(3, p.view.images_len);
    ASSERT_TRUE(66u == p.view.images[0].image_id);
    ASSERT_TRUE(77u == p.view.images[1].image_id);
    ASSERT_TRUE(88u == p.view.images[2].image_id);
}

TEST(image_view, deeply_negative_z_goes_below_the_cell_backgrounds) {
    PANE(p, 20, 10);

    feed(p, "\x1b" "[1;1H");
    transmitAndPlace(p, 61, 20, 20, "z=-2000000000");
    feed(p, "\x1b" "[3;1H");
    transmitAndPlace(p, 62, 20, 20, "z=-1");

    ASSERT_EQ(2, p.view.images_len);
    /* One placement below the backgrounds, one between them and the
     * glyphs, none in front. */
    ASSERT_EQ(1, p.view.images_bg_end);
    ASSERT_EQ(2, p.view.images_text_end);
    ASSERT_TRUE(61u == p.view.images[0].image_id);
}

/* ── Generation ──────────────────────────────────────────────────────── */

TEST(image_view, retransmitting_an_id_changes_the_generation) {
    PANE(p, 20, 10);

    transmitAndPlace(p, 71, 20, 40);
    const ScreenImagePlacement *first = placementOf(p.view, 71);
    ASSERT_TRUE(first != NULL);
    const uint64_t gen = first->generation;

    /* A move must NOT change the generation: that is the whole point of
     * the stamp, and re-uploading on every scroll would be visibly slow. */
    p.scrollViewport(0);
    const ScreenImagePlacement *moved = placementOf(p.view, 71);
    ASSERT_TRUE(moved != NULL);
    ASSERT_TRUE(gen == moved->generation);

    /* New pixels under the same ID must change it. */
    feed(p, "\x1b" "[1;1H");
    transmitAndPlace(p, 71, 20, 40);
    const ScreenImagePlacement *second = placementOf(p.view, 71);
    ASSERT_TRUE(second != NULL);
    ASSERT_TRUE(second->generation != gen);
}

TEST(image_view, serial_changes_when_a_placement_moves) {
    PANE(p, 20, 3);

    feed(p, "\x1b" "[3;1H");
    transmitAndPlace(p, 81, 20, 20);
    const uint64_t after_place = p.view.images_serial;
    ASSERT_TRUE(after_place != 0);
    ASSERT_EQ(2, (int)p.view.images[0].dest_row);

    /* An unchanged frame does not bump the serial: a repaint that
     * re-uploaded on every sync would be visibly slow. */
    p.sync();
    ASSERT_TRUE(after_place == p.view.images_serial);

    /* A newline from the bottom row scrolls the screen, which moves the
     * placement up a row and must repaint. */
    feed(p, "\r\n");
    ASSERT_TRUE(p.view.images_serial != after_place);
    ASSERT_EQ(1, (int)p.view.images[0].dest_row);
}

/* ── Pixel geometry ──────────────────────────────────────────────────── */

TEST(image_view, without_a_cell_size_nothing_is_published) {
    /* Kitty geometry is defined in pixels; with no cell size the terminal
     * cannot say how many cells an image covers, and a placement with no
     * rectangle must not be published rather than drawn at a guess. */
    PaneHolder holder(20, 5, 65536, /*with_cell_size=*/false);
    ASSERT_TRUE(holder.ok);
    VtPane &p = holder.p;

    transmitAndPlace(p, 91, 20, 40);

    ASSERT_EQ(0, p.view.images_len);
    /* The image is still in storage, and still has its bitmap. */
    ASSERT_TRUE(screen_view_has_image(&p.view, 91));

    /* Giving the pane a cell size publishes it without a retransmission,
     * which is what a font or DPI change must do. */
    p.setCellSizePx(kCellW, kCellH);
    ASSERT_EQ(1, p.view.images_len);
}

TEST(image_view, resize_keeps_the_pixel_geometry) {
    PANE(p, 20, 10);

    transmitAndPlace(p, 92, 20, 40);
    ASSERT_EQ(1, p.view.images_len);

    /* A resize that dropped the cell size would leave every placement
     * without a rectangle. */
    ASSERT_TRUE(p.resize(40, 12));
    ASSERT_EQ(1, p.view.images_len);
    const ScreenImagePlacement *pl = placementOf(p.view, 92);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(20, (int)pl->dest_width_px);
    ASSERT_EQ(40, (int)pl->dest_height_px);
}

/* ── Sub-cell offset ─────────────────────────────────────────────────── */

TEST(image_view, pixel_offset_within_the_first_cell_is_published) {
    PANE(p, 20, 10);

    /* X=/Y= offset the image inside its first cell, and are clamped to
     * one pixel less than the cell so the image cannot start in the next
     * cell. The renderer adds these to the integer cell origin. */
    transmitAndPlace(p, 101, 20, 40, "X=3,Y=7");

    const ScreenImagePlacement *pl = placementOf(p.view, 101);
    ASSERT_TRUE(pl != NULL);
    ASSERT_EQ(3, (int)pl->cell_offset_x);
    ASSERT_EQ(7, (int)pl->cell_offset_y);

    feed(p, "\x1b" "[3;1H");
    transmitAndPlace(p, 102, 20, 40, "X=500,Y=500");
    const ScreenImagePlacement *clamped = placementOf(p.view, 102);
    ASSERT_TRUE(clamped != NULL);
    ASSERT_EQ((int)kCellW - 1, (int)clamped->cell_offset_x);
    ASSERT_EQ((int)kCellH - 1, (int)clamped->cell_offset_y);
}

TEST(image_view, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
