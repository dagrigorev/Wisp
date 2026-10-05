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
