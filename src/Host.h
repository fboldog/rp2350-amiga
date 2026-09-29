// RP2350 host layer for the Omega Amiga emulator.
// Replaces the SDL2-based Host.h/Host.c with bare-metal equivalents.
//
// The framebuffer is written to PSRAM; keyboard/mouse entry points remain
// stubs until USB HID support is added.
// PicoDVI is implemented when OMEGA_ENABLE_HDMI is enabled; VGA-PIO and
// SPI-TFT remain possible alternative backends.

#pragma once
#include <stdint.h>
#include "../omega/VideoStandard.h"

// Screen dimensions that the Omega DMA engine renders into
#define SCREEN_W 640
#ifdef PICO_BUILD
// The RP2350's 8 MB PSRAM map reserves a fixed 1 MB ARGB framebuffer.
#define SCREEN_H 400
// Full-width LORES modes use alternate beam rows.  Keep the complete NTSC
// field here: a 200-row scratch buffer preserves only the upper 100 logical
// rows of the Kickstart 1.3 insert-disk artwork.
#define HOST_RASTER_H SCREEN_H
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
    int displayIsLores; // mode used by the most recently rendered bitplane row

    // Pointer to the 32-bit ARGB framebuffer in PSRAM
    void *pixels;
} Host_t;

extern Host_t host;

// Raster writes go through the host: hostRasterPixels() returns where the
// planar converter writes `width` ARGB pixels for raster (row, x), and
// hostRasterWritten() reports them afterwards. Writes along a row are
// contiguous. On the RP2350 HDMI build the host either records the written
// [min, max) columns per raster row, so the raster never needs clearing, or
// converts the pixels straight to RGB332 into the SRAM scanout frame.
#ifdef PICO_BUILD
uint32_t *hostRasterPixels(int row, int x);
void hostRasterWritten(int row, int x, int width);
#else
static inline uint32_t *hostRasterPixels(int row, int x) {
    return &((uint32_t *)host.pixels)[row * HOST_RASTER_W + x];
}
static inline void hostRasterWritten(int row, int x, int width) {
    (void)row; (void)x; (void)width;
}
#endif

// Direct RGB332 rendering (RP2350 HDMI): while hostDirectActive, the DMA
// converts plain bitplane blocks with these table-driven functions instead of
// hostRasterPixels()/hostRasterWritten() (HAM still uses the ARGB path).
#if defined(PICO_BUILD) && OMEGA_ENABLE_HDMI
extern int hostDirectActive;
// Core 1 work loop (never returns): converts the enqueued blocks into the
// scanout frame. Started by dvi_display.c after HSTX scanout is running.
void hostCore1Loop(void);
void hostDirectHires(int row, int x, uint16_t p1, uint16_t p2,
                     uint16_t p3, uint16_t p4);
void hostDirectLores(int row, int x, uint16_t p1, uint16_t p2, uint16_t p3,
                     uint16_t p4, uint16_t p5, uint16_t p6);
#else
#define hostDirectActive 0
static inline void hostDirectHires(int row, int x, uint16_t p1, uint16_t p2,
                                   uint16_t p3, uint16_t p4) {
    (void)row; (void)x; (void)p1; (void)p2; (void)p3; (void)p4;
}
static inline void hostDirectLores(int row, int x, uint16_t p1, uint16_t p2,
                                   uint16_t p3, uint16_t p4, uint16_t p5,
                                   uint16_t p6) {
    (void)row; (void)x; (void)p1; (void)p2; (void)p3; (void)p4;
    (void)p5; (void)p6;
}
#endif

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
