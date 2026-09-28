// RP2350 host layer – Phase 1 implementation.
//
// Video:   renders planar→chunky into a PSRAM framebuffer and, when enabled,
//          submits completed frames to the PicoDVI scanout running on core 1.
// Input:   USB HID via TinyUSB (stubs until USB stack is wired up).
// Serial:  printf → UART via pico_stdio_uart (configured in CMakeLists.txt).

#include "Host.h"
#include "Presentation.h"
#include "psram.h"
#include "../omega/DisplayLayout.h"
#if OMEGA_ENABLE_HDMI
#include "dvi_display.h"
#endif
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/CPU.h"
#include "../omega/VideoStandard.h"
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
// PicoDVI consumes completed frames; other display backends remain optional.
static void display_push_frame(void) {
#if OMEGA_ENABLE_HDMI
    static bool first_frame_queued;
    if (dvi_display_submit_frame(fb) && !first_frame_queued) {
        first_frame_queued = true;
        printf("DVI: first emulator frame queued\n");
    }
#else
    (void)fb;
#endif
}

// ── Lifecycle ─────────────────────────────────────────────────────────────
void hostInit(void) {
    fb = (uint32_t *)psram_ptr(PSRAM_FRAMEBUF_OFFSET);
    render_fb = (uint32_t *)psram_ptr(PSRAM_VIDEO_RASTER_OFFSET);
    host.pixels   = render_fb;
    host.rasterRow = 0;
    host.rasterX   = 0;
    host.displayIsLores = 0;
    memset(fb, 0, SCREEN_W * SCREEN_H * sizeof(uint32_t));
    memset(render_fb, 0, HOST_RASTER_PIXELS * sizeof(uint32_t));
    printf("Host init: framebuffer at %p (%d×%d ARGB)\n",
           (void *)fb, SCREEN_W, SCREEN_H);
}

void hostDisplay(void) {
    // Mouse / joystick: stub – wire up USB HID here.
    // For now, leave joy0dat alone so Workbench won't crash on NULL ptr.
#if OMEGA_ENABLE_HDMI
    if (omegaDdfIsFullWidth(chipset.ddfstrt)) {
        const uint32_t border = internal.palette[0];
        const int alternate_rows =
            host.displayIsLores &&
            omegaLoresUsesAlternateRasterRows(chipset.diwstrt,
                                               chipset.diwstop);
        const int source_step = alternate_rows ? 2 : 1;
        const int y_offset = host.displayIsLores && !alternate_rows ? 64 : 0;
        static bool first_frame_queued;
        if (dvi_display_submit_raster(
                render_fb, border, source_step, y_offset,
                omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop)) &&
            !first_frame_queued) {
            first_frame_queued = true;
            printf("DVI: first direct-raster emulator frame queued\n");
        }
        hostClearRaster(render_fb, border);
        return;
    }
#endif
    hostPresentFrame(fb, render_fb);
    display_push_frame();
}
