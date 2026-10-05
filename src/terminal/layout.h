#pragma once
#ifndef WISP_TERMINAL_LAYOUT_H
#define WISP_TERMINAL_LAYOUT_H

#include "wisp_bool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int usable_width_px;
    int usable_height_px;
    int origin_x_px;
    int origin_y_px;
    int cols;
    int rows;
} TerminalGridLayout;

TerminalGridLayout terminal_compute_grid_layout(int width_px, int height_px,
                                                float cell_w, float cell_h,
                                                int padding_x, int padding_y,
                                                int tab_bar_height);

TerminalGridLayout terminal_compute_grid_layout_ex(int width_px, int height_px,
                                                   float cell_w, float cell_h,
                                                   int padding_x, int padding_y,
                                                   int left_reserved_px,
                                                   int top_reserved_px,
                                                   int right_reserved_px,
                                                   int bottom_reserved_px);

void terminal_pixel_to_cell(const TerminalGridLayout *layout,
                            int px, int py,
                            float cell_w, float cell_h,
                            int padding_x, int padding_y,
                            int tab_bar_height,
                            int *col, int *row);

void terminal_pixel_to_cell_ex(const TerminalGridLayout *layout,
                               int px, int py,
                               float cell_w, float cell_h,
                               int padding_x, int padding_y,
                               int *col, int *row);

bool terminal_message_fully_visible(const TerminalGridLayout *layout,
                                    int line_count,
                                    bool include_prompt_line);

#ifdef __cplusplus
}
#endif

#endif
