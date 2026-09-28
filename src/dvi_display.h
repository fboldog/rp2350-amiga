#pragma once
#include <stdbool.h>
#include <stdint.h>

// Start PIO-driven standard-definition DVI using the board-specific pin map.
void dvi_display_init(void);
void dvi_display_start(void);

// Convert and queue one completed 640x400 ARGB emulator frame. Returns false
// when the previous frame is still waiting for the next DVI frame boundary.
bool dvi_display_submit_frame(const uint32_t *argb_frame);

// Fast path for full-width Amiga modes: present the DMA beam raster directly,
// avoiding the intermediate 640x400 ARGB framebuffer in PSRAM. Only the
// [row_min, row_max) columns of each raster row were written this frame; all
// other pixels are shown in the border colour without being read.
bool dvi_display_submit_raster(const uint32_t *argb_raster,
                               const int16_t *row_min,
                               const int16_t *row_max,
                               uint32_t border,
                               int source_step,
                               int y_offset,
                               int row_rotation);
