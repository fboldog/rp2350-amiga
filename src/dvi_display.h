#pragma once
#include <stdbool.h>
#include <stdint.h>

// Start HSTX standard-definition DVI output on the board's HDMI pins.
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

// Direct RGB332 rendering. The emulator writes converted pixels straight
// into the free SRAM scanout frame instead of the ARGB raster in PSRAM.
#define DVI_DIRECT_IMAGE_WIDTH 640
// Image rows of the direct frame (240 NTSC, 256 PAL).
int dvi_display_direct_height(void);
// Waits (at most one DVI frame) for the back buffer and returns its
// 640-pixel-stride image, or NULL while the boot pattern is still shown.
uint8_t *dvi_display_direct_acquire(void);
// Queues the acquired frame with the given ARGB border colour.
void dvi_display_direct_publish(uint32_t border);

static inline uint8_t dvi_rgb332(uint32_t argb) {
    return (uint8_t)(((argb >> 16) & 0xe0u) |
                     ((argb >> 11) & 0x1cu) |
                     ((argb >> 6) & 0x03u));
}
