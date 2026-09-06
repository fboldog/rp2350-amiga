// RP2350 host layer for the Omega Amiga emulator.
// Replaces the SDL2-based Host.h/Host.c with bare-metal equivalents.
//
// Phase 1 (this file): framebuffer written to PSRAM; UART for debug output;
//   USB HID stubs for keyboard/mouse.
// Phase 2: hook up PicoDVI / VGA-PIO / SPI-TFT for real video output.

#pragma once
#include <stdint.h>
#include "../omega/VideoStandard.h"

// Screen dimensions that the Omega DMA engine renders into
#define SCREEN_W 640
#ifdef PICO_BUILD
// The RP2350's 8 MB PSRAM map reserves a fixed 1 MB ARGB framebuffer.
#define SCREEN_H 400
#define HOST_RASTER_H 200
#else
#define SCREEN_H OMEGA_VIDEO_NATIVE_HEIGHT
// Native LORES DMA can occupy every PAL/NTSC beam row before presentation
// deinterlaces it.  Keep the complete field so artwork near the bottom is
// not truncated; HIRES presentation still consumes only SCREEN_H / 2 rows.
#define HOST_RASTER_H SCREEN_H
#endif

// Omega's DMA renderer produces an intermediate beam raster.  Kickstart's
// DMA fetch window contains pipeline/overscan words around the visible data.
// The host presentation step clips that raster into a 640x400 image and
// doubles scanlines vertically.
#define HOST_RASTER_W       640
#define HOST_RASTER_PIXELS  (HOST_RASTER_W * HOST_RASTER_H)
#define HOST_VISIBLE_X0     40
#define HOST_VISIBLE_X1     596
#define HOST_FETCH_LEAD     24
#define HOST_CONTENT_Y      44

// Host state – RP2350 flavour (no SDL types here)
typedef struct {
    int rasterRow;  // destination row in the intermediate DMA raster
    int rasterX;    // destination pixel within rasterRow
    int vblCount;   // vertical blank counter
    int displayIsLores; // mode used by the most recently rendered bitplane row

    // Pointer to the 32-bit ARGB framebuffer in PSRAM
    void *pixels;
} Host_t;

extern Host_t host;

// ── Pixel-conversion helpers (same signature as the SDL version) ──────────
void hiresPlanar2Chunky(uint32_t *pixBuff,
                        uint16_t p1, uint16_t p2, uint16_t p3, uint16_t p4);
void loresPlanar2Chunky(uint32_t *pixBuff,
                        uint16_t p1, uint16_t p2, uint16_t p3, uint16_t p4,
                        uint16_t p5, uint16_t p6);
void loresHAM2Chunky(uint32_t *pixBuff,
                     uint16_t p1, uint16_t p2, uint16_t p3, uint16_t p4,
                     uint16_t p5, uint16_t p6);
// ── Keyboard / mouse ──────────────────────────────────────────────────────
void pressKey(uint16_t keyCode);
void releaseKey(uint16_t keyCode);

// ── Lifecycle ─────────────────────────────────────────────────────────────
void hostInit(void);
// Called once per frame: push framebuffer to display hardware, poll input
void hostDisplay(void);

void toggleLEDs(void);
