#pragma once
#include <stdbool.h>
#include <stdint.h>

// Start PIO-driven standard-definition DVI using the board-specific pin map.
void dvi_display_init(void);
void dvi_display_start(void);

// Convert and queue one completed 640x400 ARGB emulator frame. Returns false
// when the previous frame is still waiting for the next DVI frame boundary.
bool dvi_display_submit_frame(const uint32_t *argb_frame);
