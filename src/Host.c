// RP2350 host layer – Phase 1 implementation.
//
// Video:   renders planar→chunky into a PSRAM framebuffer; no real display
//          output yet.  Wire up PicoDVI / VGA PIO in Phase 2 by implementing
//          display_push_frame() below.
// Input:   USB HID via TinyUSB (stubs until USB stack is wired up).
// Serial:  printf → UART via pico_stdio_uart (configured in CMakeLists.txt).

#include "Host.h"
#include "psram.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/CPU.h"
#include "pico/stdlib.h"
#include <string.h>

Host_t host;

// Framebuffer lives in PSRAM
static uint32_t *fb;        // presented SCREEN_W * SCREEN_H image
static uint32_t *render_fb; // intermediate DMA beam raster

// ── Amiga key-code table (same values as the SDL Host.c) ─────────────────
static const uint8_t keyMapping[] = {
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x41, 0x42, 0x0,  0x0,  0x0,  0x44, 0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x45, 0x0,  0x0,  0x0,  0x0,
    0x40, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x2A,
    0x0,  0x0,  0x0,  0x0,  0x38, 0x0B, 0x39, 0x3A,
    0x0A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0,  0x29, 0x52, 0x0C, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x46, 0x5F, 0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x4E,
    0x4F, 0x4D, 0x4C, 0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x1A, 0x2B, 0x1B, 0x0,  0x0,
    0x0,  0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24,
    0x25, 0x17, 0x26, 0x27, 0x28, 0x37, 0x36, 0x18,
    0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11,
    0x32, 0x15, 0x31, 0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,
    0x0,  0x0,  0x0,  0x30,
};

static int LEDActive = 1;
static int keyMask   = 0;

void pressKey(uint16_t keyCode) {
    if (keyCode >= sizeof(keyMapping)) return;
    CIAWrite(&CIAA, 0xC, ~(keyMapping[keyCode] << 1));
    keyboardInt();

    if (keyCode == 224) keyMask |= 0x1;  // Ctrl
    if (keyCode == 227) keyMask |= 0x2;  // Left Amiga
    if (keyCode == 231) keyMask |= 0x4;  // Right Amiga
    if (keyMask == 0x7) cpu_pulse_reset();
}

void releaseKey(uint16_t keyCode) {
    if (keyCode >= sizeof(keyMapping)) return;
    CIAWrite(&CIAA, 0xC, ~((keyMapping[keyCode] << 1) | 1));
    keyboardInt();

    if (keyCode == 224) keyMask &= 0x6;
    if (keyCode == 227) keyMask &= 0x5;
    if (keyCode == 231) keyMask &= 0x3;
}

void toggleLEDs(void) {
    LEDActive = 1 - LEDActive;
}

// ── Display output stub ───────────────────────────────────────────────────
// Replace this with PicoDVI push / SPI TFT blit / VGA scanline feed.
static void display_push_frame(void) {
    // Phase 1: no-op – framebuffer is in PSRAM, hook up display here.
    // Example for SPI TFT: spi_tft_blit(fb, SCREEN_W, SCREEN_H);
    (void)fb;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────
void hostInit(void) {
    fb = (uint32_t *)psram_ptr(PSRAM_FRAMEBUF_OFFSET);
    render_fb = (uint32_t *)psram_ptr(PSRAM_VIDEO_RASTER_OFFSET);
    host.pixels   = render_fb;
    host.FBCounter = 0;
    host.vblCount  = 0;
    memset(fb, 0, SCREEN_W * SCREEN_H * sizeof(uint32_t));
    memset(render_fb, 0, HOST_RASTER_PIXELS * sizeof(uint32_t));
    printf("Host init: framebuffer at %p (%d×%d ARGB)\n",
           (void *)fb, SCREEN_W, SCREEN_H);
}

void hostDisplay(void) {
    // Mouse / joystick: stub – wire up USB HID here.
    // For now, leave joy0dat alone so Workbench won't crash on NULL ptr.

    const uint32_t border = internal.palette[0];
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i)
        fb[i] = border;
    for (int sy = 0; sy < HOST_RASTER_H; ++sy) {
        int dy = HOST_CONTENT_Y + sy * 2;
        for (int x = HOST_VISIBLE_X0; x < HOST_VISIBLE_X1; ++x) {
            int sx = HOST_FETCH_LEAD + x;
            uint32_t pixel = render_fb[sy * HOST_RASTER_W + sx];
            fb[dy * SCREEN_W + x] = pixel;
            fb[(dy + 1) * SCREEN_W + x] = pixel;
        }
    }
    for (int i = 0; i < HOST_RASTER_PIXELS; ++i)
        render_fb[i] = border;
    display_push_frame();
}

// ── Planar → Chunky pixel conversion (unchanged from original Host.c) ─────

void hiresPlanar2Chunky(uint32_t *pixBuff, uint32_t *palette,
                        uint16_t plane1, uint16_t plane2,
                        uint16_t plane3, uint16_t plane4) {
    int counter = host.FBCounter;
    for (int j = 7; j > -1; --j) {
        uint32_t c1 =  (plane1 >> j) & 1;
        c1 |= (((plane2 >> j) & 1) << 1);
        c1 |= (((plane3 >> j) & 1) << 2);
        c1 |= (((plane4 >> j) & 1) << 3);

        int k = j + 8;
        uint32_t c2 =  (plane1 >> k) & 1;
        c2 |= (((plane2 >> k) & 1) << 1);
        c2 |= (((plane3 >> k) & 1) << 2);
        c2 |= (((plane4 >> k) & 1) << 3);

        pixBuff[counter]     = internal.palette[c1];
        pixBuff[counter + 8] = internal.palette[c2];
        counter++;
    }
}

void loresPlanar2Chunky(uint32_t *pixBuff, uint32_t *palette,
                        uint16_t plane1, uint16_t plane2,
                        uint16_t plane3, uint16_t plane4,
                        uint16_t plane5, uint16_t plane6) {
    int counter = host.FBCounter;
    for (int j = 7; j > -1; --j) {
        uint32_t c1 =  (plane1 >> j) & 1;
        c1 |= (((plane2 >> j) & 1) << 1);
        c1 |= (((plane3 >> j) & 1) << 2);
        c1 |= (((plane4 >> j) & 1) << 3);
        c1 |= (((plane5 >> j) & 1) << 4);
        c1 |= (((plane6 >> j) & 1) << 5);

        int k = j + 8;
        uint32_t c2 =  (plane1 >> k) & 1;
        c2 |= (((plane2 >> k) & 1) << 1);
        c2 |= (((plane3 >> k) & 1) << 2);
        c2 |= (((plane4 >> k) & 1) << 3);
        c2 |= (((plane5 >> k) & 1) << 4);
        c2 |= (((plane6 >> k) & 1) << 5);

        uint32_t col1 = internal.palette[c1];
        uint32_t col2 = internal.palette[c2];
        pixBuff[counter]      = col1;
        pixBuff[counter + 16] = col2;
        counter++;
        pixBuff[counter]      = col1;
        pixBuff[counter + 16] = col2;
        counter++;
    }
}

void loresHAM2Chunky(uint32_t *pixBuff, uint32_t *palette,
                     uint16_t plane1, uint16_t plane2,
                     uint16_t plane3, uint16_t plane4,
                     uint16_t plane5, uint16_t plane6) {
    int counter = host.FBCounter;

    for (int j = 7; j > -1; --j) {
        uint32_t c1 =  (plane1 >> j) & 1;
        c1 |= (((plane2 >> j) & 1) << 1);
        c1 |= (((plane3 >> j) & 1) << 2);
        c1 |= (((plane4 >> j) & 1) << 3);
        c1 |= (((plane5 >> j) & 1) << 4);
        c1 |= (((plane6 >> j) & 1) << 5);

        if (c1 & 0xF0) {
            int      ctrl  = c1 >> 4;
            uint32_t col   = c1 & 0xF;
            col = (col << 4) | col;
            uint32_t prev  = (counter > 0) ? pixBuff[counter - 1] : 0;
            switch (ctrl) {
                case 1: prev = (prev & 0xFFFFFF00u) |  col;          break;
                case 2: prev = (prev & 0xFF00FFFFu) | (col << 16);   break;
                case 3: prev = (prev & 0xFFFF00FFu) | (col <<  8);   break;
            }
            pixBuff[counter++] = prev;
            pixBuff[counter++] = prev;
        } else {
            uint32_t colour = internal.palette[c1];
            pixBuff[counter++] = colour;
            pixBuff[counter++] = colour;
        }
    }

    counter = host.FBCounter;
    for (int j = 7; j > -1; --j) {
        int k = j + 8;
        uint32_t c2 =  (plane1 >> k) & 1;
        c2 |= (((plane2 >> k) & 1) << 1);
        c2 |= (((plane3 >> k) & 1) << 2);
        c2 |= (((plane4 >> k) & 1) << 3);
        c2 |= (((plane5 >> k) & 1) << 4);
        c2 |= (((plane6 >> k) & 1) << 5);

        if (c2 & 0xF0) {
            int      ctrl  = c2 >> 4;
            uint32_t col   = c2 & 0xF;
            col = (col << 4) | col;
            uint32_t prev  = pixBuff[counter + 15];
            switch (ctrl) {
                case 1: prev = (prev & 0xFFFFFF00u) |  col;          break;
                case 2: prev = (prev & 0xFF00FFFFu) | (col << 16);   break;
                case 3: prev = (prev & 0xFFFF00FFu) | (col <<  8);   break;
            }
            pixBuff[counter + 16] = prev;  counter++;
            pixBuff[counter + 16] = prev;  counter++;
        } else {
            uint32_t colour = internal.palette[c2];
            pixBuff[counter + 16] = colour; counter++;
            pixBuff[counter + 16] = colour; counter++;
        }
    }
}

void sprite2chunky(uint32_t *pixBuff, uint32_t *palette, int x,
                   uint16_t plane1, uint16_t plane2, int delta) {
    int counter = x;
    for (int j = 7; j > -1; --j) {
        uint32_t c1 =  (plane1 >> j) & 1;
        c1 |= (((plane2 >> j) & 1) << 1);
        int k = j + 8;
        uint32_t c2 =  (plane1 >> k) & 1;
        c2 |= (((plane2 >> k) & 1) << 1);

        // Clamp BOTH ends: a sprite parked off the left edge gives a negative
        // counter (KS 3.1 does this with sprite 0), which would write before
        // the framebuffer.  Upstream only checked the upper bound.
        if (c1 > 0 && counter >= 0 && counter < SCREEN_W)
            pixBuff[counter] = palette[c1];
        if (c2 > 0 && (counter + delta) >= 0 && (counter + delta) < SCREEN_W)
            pixBuff[counter + delta] = palette[c2];
        counter++;

        if (delta == 16) {
            if (c1 > 0 && counter >= 0 && counter < SCREEN_W)
                pixBuff[counter] = palette[c1];
            if (c2 > 0 && (counter + delta) >= 0 && (counter + delta) < SCREEN_W)
                pixBuff[counter + delta] = palette[c2];
            counter++;
        }
    }
}
