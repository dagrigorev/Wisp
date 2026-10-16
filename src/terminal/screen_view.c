#include "screen_view.h"

#include <stdlib.h>
#include <string.h>

static ScreenCell *alloc_grid(int cols, int rows) {
    const size_t n = (size_t)cols * (size_t)rows;
    ScreenCell *cells = (ScreenCell *)calloc(n ? n : 1, sizeof(ScreenCell));
    if (!cells) return NULL;
    const ScreenCell blank = screen_cell_blank();
    for (size_t i = 0; i < n; i++) cells[i] = blank;
    return cells;
}

bool screen_view_init(ScreenBuffer *sb, int cols, int rows) {
    if (!sb) return false;
    memset(sb, 0, sizeof(*sb));
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    sb->cells = alloc_grid(cols, rows);
    if (!sb->cells) return false;

    sb->styles = style_table_create();
    if (!sb->styles) {
        free(sb->cells);
        sb->cells = NULL;
        return false;
    }

    sb->cols = cols;
    sb->rows = rows;
    sb->cursor_visible = true;
    return true;
}

void screen_view_free(ScreenBuffer *sb) {
    if (!sb) return;
    free(sb->cells);
    sb->cells = NULL;
    free(sb->images);
    sb->images = NULL;
    sb->images_len = sb->images_cap = 0;
    sb->images_bg_end = sb->images_text_end = 0;
    free(sb->image_ids);
    sb->image_ids = NULL;
    sb->image_ids_len = sb->image_ids_cap = 0;
    if (sb->styles) {
        style_table_destroy(sb->styles);
        sb->styles = NULL;
    }
    sb->cols = 0;
    sb->rows = 0;
}

bool screen_view_resize(ScreenBuffer *sb, int cols, int rows) {
    if (!sb) return false;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (sb->cols == cols && sb->rows == rows) return true;

    ScreenCell *cells = alloc_grid(cols, rows);
    if (!cells) return false;

    free(sb->cells);
    sb->cells = cells;
    sb->cols = cols;
    sb->rows = rows;

    /* The cursor may be outside the new grid until the next sync. */
    if (sb->cursor_x >= cols) sb->cursor_x = cols - 1;
    if (sb->cursor_y >= rows) sb->cursor_y = rows - 1;
    return true;
}

const ScreenCell *screen_visible_cell(const ScreenBuffer *sb, int viewport_row, int col) {
    if (!sb || !sb->cells) return NULL;
    if (viewport_row < 0 || col < 0) return NULL;
    if (viewport_row >= sb->rows || col >= sb->cols) return NULL;
    return sb->cells + (size_t)viewport_row * (size_t)sb->cols + (size_t)col;
}

int screen_max_viewport_offset(const ScreenBuffer *sb) {
    if (!sb) return 0;
    return sb->scrollback_count;
}

/* ── Images ──────────────────────────────────────────────────────────────
 *
 * Original to Wisp. The placement list is rebuilt from scratch each sync,
 * which is what Ghostty's renderer/image.zig `kittyUpdate` does too
 * (`clearRetainingCapacity` then re-walk the storage): placements are few,
 * and any of them can move on any scroll, so there is nothing cheaper to
 * diff. The arrays are grow-only across frames for the same reason the
 * style table is. */

static bool images_reserve(ScreenBuffer *sb, int want) {
    int cap;
    ScreenImagePlacement *grown;
    if (want <= sb->images_cap) return true;
    cap = sb->images_cap ? sb->images_cap * 2 : 8;
    while (cap < want) cap *= 2;
    grown = (ScreenImagePlacement *)realloc(sb->images, (size_t)cap * sizeof(*grown));
    if (!grown) return false;
    sb->images = grown;
    sb->images_cap = cap;
    return true;
}

static bool image_ids_reserve(ScreenBuffer *sb, int want) {
    int cap;
    uint32_t *grown;
    if (want <= sb->image_ids_cap) return true;
    cap = sb->image_ids_cap ? sb->image_ids_cap * 2 : 8;
    while (cap < want) cap *= 2;
    grown = (uint32_t *)realloc(sb->image_ids, (size_t)cap * sizeof(*grown));
    if (!grown) return false;
    sb->image_ids = grown;
    sb->image_ids_cap = cap;
    return true;
}

void screen_view_images_begin(ScreenBuffer *sb) {
    if (!sb) return;
    sb->images_len = 0;
    sb->images_bg_end = 0;
    sb->images_text_end = 0;
    sb->image_ids_len = 0;
}

bool screen_view_images_push(ScreenBuffer *sb, const ScreenImagePlacement *p) {
    if (!sb || !p) return false;
    if (!images_reserve(sb, sb->images_len + 1)) return false;
    sb->images[sb->images_len++] = *p;
    return true;
}

bool screen_view_images_push_id(ScreenBuffer *sb, uint32_t image_id) {
    if (!sb) return false;
    if (!image_ids_reserve(sb, sb->image_ids_len + 1)) return false;
    sb->image_ids[sb->image_ids_len++] = image_id;
    return true;
}

/* Sort key: z first, then image ID. The ID tiebreak is upstream's
 * `Id.zLessThan`, and it exists for the same reason: the storage's hash map
 * iterates in an unspecified order, so without it two images at the same z
 * could swap draw order between frames and flicker. */
static int placement_less(const ScreenImagePlacement *a, const ScreenImagePlacement *b) {
    if (a->z != b->z) return a->z < b->z;
    return a->image_id < b->image_id;
}

static void placements_sort(ScreenImagePlacement *a, int n) {
    /* Insertion sort: a screen holds a handful of placements, and this is
     * stable, which keeps the order of two placements of the same image at
     * the same z fixed from frame to frame. */
    int i, j;
    for (i = 1; i < n; i++) {
        const ScreenImagePlacement key = a[i];
        for (j = i - 1; j >= 0 && placement_less(&key, &a[j]); j--) a[j + 1] = a[j];
        a[j + 1] = key;
    }
}

void screen_view_images_end(ScreenBuffer *sb) {
    /* Kitty's "below the cell backgrounds" tier. Upstream uses
     * minInt(i32)/2 for this and so do we, so a client that targets
     * Ghostty's behavior gets the same split here. */
    const int32_t bg_limit = (-2147483647 - 1) / 2;
    uint64_t hash = 1469598103934665603u; /* FNV-1a offset basis */
    int i;

    if (!sb) return;

    placements_sort(sb->images, sb->images_len);

    sb->images_bg_end = sb->images_len;
    sb->images_text_end = sb->images_len;
    for (i = 0; i < sb->images_len; i++) {
        if (sb->images[i].z >= bg_limit && sb->images_bg_end > i) sb->images_bg_end = i;
        if (sb->images[i].z >= 0) {
            sb->images_text_end = i;
            break;
        }
    }
    if (sb->images_bg_end > sb->images_text_end) sb->images_bg_end = sb->images_text_end;

    /* Hash the published geometry so an unchanged frame does not bump the
     * serial. The pixel pointer is deliberately not hashed: it can move
     * without the image changing, and `generation` is the authority on
     * whether the pixels differ. */
    for (i = 0; i < sb->images_len; i++) {
        const ScreenImagePlacement *p = &sb->images[i];
        const uint32_t words[12] = {
            p->image_id,       (uint32_t)(p->generation & 0xFFFFFFFFu),
            (uint32_t)(p->generation >> 32), (uint32_t)p->dest_col,
            (uint32_t)p->dest_row, p->cell_offset_x,
            p->cell_offset_y,  p->dest_width_px,
            p->dest_height_px, p->source_x,
            p->source_y,       (uint32_t)p->z,
        };
        size_t w;
        for (w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
            hash ^= (uint64_t)words[w];
            hash *= 1099511628211u;
        }
        hash ^= (uint64_t)p->source_width ^ ((uint64_t)p->source_height << 32);
        hash *= 1099511628211u;
    }
    if (hash != sb->images_hash) {
        sb->images_hash = hash;
        sb->images_serial++;
    }
}

bool screen_view_has_image(const ScreenBuffer *sb, uint32_t image_id) {
    int i;
    if (!sb) return false;
    for (i = 0; i < sb->image_ids_len; i++)
        if (sb->image_ids[i] == image_id) return true;
    return false;
}
