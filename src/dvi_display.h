#pragma once

// Start the Waveshare RP2350-PiZero's PIO-driven standard-definition DVI.
// The HDMI connector is wired to GPIO32..39, which are PIO-capable but are
// not connected to the RP2350 HSTX peripheral.
void dvi_display_init(void);
