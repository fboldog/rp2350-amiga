#pragma once
#include <stdbool.h>
#include <stdint.h>

// TMDS bit rate of the selected video mode (252 MHz NTSC, 270 MHz PAL).
// main.c runs PLL_USB at this rate; HSTX takes clk_hstx = PLL_USB / 2.
uint32_t dvi_display_bit_clock_khz(void);

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

// Direct indexed rendering (exact Amiga colours). Core 1 writes colour
// register numbers straight into the free SRAM scanout frame: 0..31, 32..63
// for extra half-brite, or raw 6-bit HAM codes on HAM segments. It records
// each run of pixels it draws with the number of palette writes logged
// before it; scanout replays the log and expands every row to RGB888, so
// Copper palette changes, even mid-line, keep their exact 12-bit colours.
#define DVI_DIRECT_IMAGE_WIDTH 640
#define DVI_MAX_ROWS 256
#define DVI_MAX_SEGMENTS 1024
#define DVI_MAX_PALETTE_LOG 2047  // DVI_SEGMENT_POS is 11 bits
// Segment: image columns [x0, x1) of one row, drawn after `pos` palette-log
// entries; HAM decodes the pixels as HAM codes.
#define DVI_SEGMENT(x0, x1, pos, ham) \
    ((uint32_t)(x0) | (uint32_t)(x1) << 10 | (uint32_t)(pos) << 20 | \
     (uint32_t)(ham) << 31)
#define DVI_SEGMENT_X0(s)  ((s) & 0x3ffu)
#define DVI_SEGMENT_X1(s)  (((s) >> 10) & 0x3ffu)
#define DVI_SEGMENT_POS(s) (((s) >> 20) & 0x7ffu)
#define DVI_SEGMENT_HAM(s) ((s) >> 31)
typedef struct {
    uint16_t initial_palette[32];         // 0x0RGB colours at frame begin
    // Segments of image row y: [row_segment[y], row_segment[y + 1]), in the
    // order they were drawn. A row without segments shows its border colour.
    uint16_t row_segment[DVI_MAX_ROWS + 1];
    // Palette-log position at the end of each row's line: a row without
    // segments shows COLOR00 as of then (Copper colour bars in the border).
    uint16_t row_log[DVI_MAX_ROWS];
    uint16_t segments_used;
    uint16_t log_used;
    bool segment_overflow;                // rows past the overflow: no segments
    uint32_t segment[DVI_MAX_SEGMENTS];
    uint32_t palette_log[DVI_MAX_PALETTE_LOG]; // register << 12 | 0x0RGB
} dvi_indexed_frame_t;

// Image rows of the direct frame (240 NTSC, 256 PAL).
int dvi_display_direct_height(void);
// True while a finished frame waits for the next DVI frame boundary.
bool dvi_display_frame_pending(void);
// Claims the back buffer and returns its 640-pixel-stride image and its
// segment/palette record, or NULL while the boot pattern is still shown.
// Call only when no frame is pending. Called on core 1.
uint8_t *dvi_display_direct_acquire(dvi_indexed_frame_t **record);
// Queues the acquired frame; `border` is COLOR00 (0x0RGB) at frame end.
void dvi_display_direct_publish(uint16_t border);

static inline uint8_t dvi_rgb332(uint32_t argb) {
    return (uint8_t)(((argb >> 16) & 0xe0u) |
                     ((argb >> 11) & 0x1cu) |
                     ((argb >> 6) & 0x03u));
}

// Display refreshes so far (incremented by the scanout at each frame start).
extern volatile uint32_t dvi_refreshes;
