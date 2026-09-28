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

int16_t hostRasterRowMin[HOST_RASTER_H];
int16_t hostRasterRowMax[HOST_RASTER_H];
// Set while render_fb may hold stale pixels outside the written ranges.
static bool raster_has_stale_pixels;

static void hostResetRasterWritten(void) {
    for (int row = 0; row < HOST_RASTER_H; ++row) {
        hostRasterRowMin[row] = HOST_RASTER_W;
        hostRasterRowMax[row] = 0;
    }
}

// The presentation fallback expects every unwritten raster pixel to hold
// the border colour. Restore that after frames that skipped the clear.
static void hostBorderUnwrittenPixels(uint32_t border) {
    for (int row = 0; row < HOST_RASTER_H; ++row) {
        uint32_t *line = &render_fb[row * HOST_RASTER_W];
        int written_min = hostRasterRowMin[row];
        int written_max = hostRasterRowMax[row];
        if (written_max <= written_min)
            written_min = written_max = HOST_RASTER_W;
        for (int x = 0; x < written_min; ++x)
            line[x] = border;
        for (int x = written_max; x < HOST_RASTER_W; ++x)
            line[x] = border;
    }
}

#if OMEGA_ENABLE_HDMI
// Direct RGB332 rendering: for full-width fetch layouts on boards with SRAM
// scanout frames, converted pixels go straight into the free scanout frame,
// so the ARGB raster in PSRAM is neither written nor read. The layout (the
// same mapping dvi_display_submit_raster() applies) is captured at frame
// start.
#define DIRECT_MAX_ROWS 256
static bool direct_frame;         // this frame renders straight to RGB332
int hostDirectActive;             // mirrors direct_frame for DMA.c
static uint8_t *direct_image;     // acquired scanout frame, or NULL
static int direct_first_row;      // image row of raster row 0
static int direct_step;           // raster rows per image row
static int direct_rotation;       // DDF row rotation in raster columns
static int direct_height;         // image rows
static int16_t direct_row_min[DIRECT_MAX_ROWS];
static int16_t direct_row_max[DIRECT_MAX_ROWS];
static uint32_t direct_block[32]; // one planar block of ARGB pixels

static void hostDirectResetRows(void) {
    for (int y = 0; y < DIRECT_MAX_ROWS; ++y) {
        direct_row_min[y] = DVI_DIRECT_IMAGE_WIDTH;
        direct_row_max[y] = 0;
    }
}

static void hostDirectBegin(void) {
    direct_frame = dvi_display_direct_supported() &&
                   omegaDdfIsFullWidth(chipset.ddfstrt);
    hostDirectActive = direct_frame;
    if (!direct_frame)
        return;
    const int alternate_rows =
        host.displayIsLores &&
        omegaLoresUsesAlternateRasterRows(chipset.diwstrt, chipset.diwstop);
    direct_step = alternate_rows ? 2 : 1;
    // dvi_display_submit_raster() skips y_offset / 2 image rows (64 / 2).
    direct_first_row = host.displayIsLores && !alternate_rows ? 32 : 0;
    direct_rotation = omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop);
    direct_height = dvi_display_direct_height();
    if (direct_height > DIRECT_MAX_ROWS)
        direct_height = DIRECT_MAX_ROWS;
}

static inline void hostDirectCopy(uint8_t *line, const uint8_t *pixels,
                                  int y, int x, int count) {
    memcpy(line + x, pixels, (size_t)count);
    if (x < direct_row_min[y]) direct_row_min[y] = (int16_t)x;
    if (x + count > direct_row_max[y])
        direct_row_max[y] = (int16_t)(x + count);
}

// Places `width` RGB332 pixels of raster (row, x) into the scanout frame.
static void hostDirectPlace(int row, int x, const uint8_t *pixels, int width) {
    if (!direct_image) {
        direct_image = dvi_display_direct_acquire();
        if (!direct_image)
            return; // boot pattern still shown
    }
    if (row % direct_step)
        return;
    const int y = direct_first_row + row / direct_step;
    if (y >= direct_height)
        return;
    uint8_t *line = direct_image + y * DVI_DIRECT_IMAGE_WIDTH;
    int image_x = x - direct_rotation;
    if (image_x < 0)
        image_x += HOST_RASTER_W;
    int first = HOST_RASTER_W - image_x;
    if (first > width)
        first = width;
    hostDirectCopy(line, pixels, y, image_x, first);
    if (first < width) // the block wraps around the rotated row
        hostDirectCopy(line, pixels + first, y, 0, width - first);
}

// ARGB block (HAM path) -> RGB332.
static void hostDirectCommit(int row, int x, int width) {
    uint8_t pixels[32];
    for (int i = 0; i < width; ++i)
        pixels[i] = dvi_rgb332(direct_block[i]);
    hostDirectPlace(row, x, pixels, width);
}

// Table-driven planar-to-RGB332: c2p_spread[b] holds the 8 pixels of plane
// byte b as bytes 0/1 (leftmost pixel = bit 7 = lowest byte), so OR-ing the
// entries of all planes, shifted by plane number, yields 8 colour indices
// at once. Pixel order matches *Planar2Chunky: bits 7..0 of the low byte of
// each plane word first, then bits 15..8.
static uint32_t c2p_spread[256][2];

static void hostInitPlanarTables(void) {
    for (int b = 0; b < 256; ++b) {
        uint32_t lo = 0, hi = 0;
        for (int k = 0; k < 4; ++k) {
            lo |= (uint32_t)((b >> (7 - k)) & 1) << (8 * k);
            hi |= (uint32_t)((b >> (3 - k)) & 1) << (8 * k);
        }
        c2p_spread[b][0] = lo;
        c2p_spread[b][1] = hi;
    }
}

#define C2P(p, shift, half) c2p_spread[((p) >> (shift)) & 0xffu][half]

void hostDirectHires(int row, int x, uint16_t p1, uint16_t p2,
                     uint16_t p3, uint16_t p4) {
    uint8_t pixels[16];
    const uint8_t *palette = internal.palette332;
    for (int half = 0; half < 2; ++half) {
        const int shift = half * 8;
        for (int part = 0; part < 2; ++part) {
            uint32_t index = C2P(p1, shift, part) |
                             C2P(p2, shift, part) << 1 |
                             C2P(p3, shift, part) << 2 |
                             C2P(p4, shift, part) << 3;
            uint8_t *out = pixels + half * 8 + part * 4;
            out[0] = palette[index & 0xffu];
            out[1] = palette[(index >> 8) & 0xffu];
            out[2] = palette[(index >> 16) & 0xffu];
            out[3] = palette[index >> 24];
        }
    }
    hostDirectPlace(row, x, pixels, 16);
}

// LORES: 16 pixels of up to 6 planes (EHB palette 32..63), each doubled.
void hostDirectLores(int row, int x, uint16_t p1, uint16_t p2, uint16_t p3,
                     uint16_t p4, uint16_t p5, uint16_t p6) {
    uint8_t pixels[32];
    const uint8_t *palette = internal.palette332;
    for (int half = 0; half < 2; ++half) {
        const int shift = half * 8;
        for (int part = 0; part < 2; ++part) {
            uint32_t index = C2P(p1, shift, part) |
                             C2P(p2, shift, part) << 1 |
                             C2P(p3, shift, part) << 2 |
                             C2P(p4, shift, part) << 3 |
                             C2P(p5, shift, part) << 4 |
                             C2P(p6, shift, part) << 5;
            uint8_t *out = pixels + half * 16 + part * 8;
            for (int k = 0; k < 4; ++k) {
                const uint8_t colour = palette[(index >> (8 * k)) & 0xffu];
                out[2 * k] = colour;
                out[2 * k + 1] = colour;
            }
        }
    }
    hostDirectPlace(row, x, pixels, 32);
}

static void hostDirectFinish(void) {
    if (!direct_image)
        direct_image = dvi_display_direct_acquire(); // e.g. bitplanes off
    if (direct_image) {
        const uint8_t border = dvi_rgb332(internal.palette[0]);
        for (int y = 0; y < direct_height; ++y) {
            uint8_t *line = direct_image + y * DVI_DIRECT_IMAGE_WIDTH;
            int written_min = direct_row_min[y];
            int written_max = direct_row_max[y];
            if (written_max <= written_min)
                written_min = written_max = DVI_DIRECT_IMAGE_WIDTH;
            memset(line, border, (size_t)written_min);
            memset(line + written_max, border,
                   (size_t)(DVI_DIRECT_IMAGE_WIDTH - written_max));
        }
        dvi_display_direct_publish(internal.palette[0]);
        static bool first_frame_queued;
        if (!first_frame_queued) {
            first_frame_queued = true;
            printf("DVI: first direct RGB332 emulator frame queued\n");
        }
    }
    direct_image = NULL;
    hostDirectResetRows();
}
#endif

uint32_t *hostRasterPixels(int row, int x) {
#if OMEGA_ENABLE_HDMI
    if (direct_frame)
        return direct_block;
#endif
    return &render_fb[row * HOST_RASTER_W + x];
}

void hostRasterWritten(int row, int x, int width) {
#if OMEGA_ENABLE_HDMI
    if (direct_frame) {
        hostDirectCommit(row, x, width);
        return;
    }
#endif
    if (x < hostRasterRowMin[row]) hostRasterRowMin[row] = (int16_t)x;
    if (x + width > hostRasterRowMax[row])
        hostRasterRowMax[row] = (int16_t)(x + width);
}

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
    hostResetRasterWritten();
#if OMEGA_ENABLE_HDMI
    hostDirectResetRows();
    direct_frame = false;
    hostDirectActive = 0;
    hostInitPlanarTables();
#endif
    printf("Host init: framebuffer at %p (%d×%d ARGB)\n",
           (void *)fb, SCREEN_W, SCREEN_H);
}

void hostDisplay(void) {
    // Mouse / joystick: stub – wire up USB HID here.
    // For now, leave joy0dat alone so Workbench won't crash on NULL ptr.
#if OMEGA_ENABLE_HDMI
    if (direct_frame) {
        hostDirectFinish();
        // The ARGB raster was not written; older contents are stale.
        raster_has_stale_pixels = true;
        hostResetRasterWritten();
        hostDirectBegin();
        return;
    }
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
                render_fb, hostRasterRowMin, hostRasterRowMax,
                border, source_step, y_offset,
                omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop)) &&
            !first_frame_queued) {
            first_frame_queued = true;
            printf("DVI: first direct-raster emulator frame queued\n");
        }
        // No 1 MB raster clear: the next frame reads only what it writes.
        raster_has_stale_pixels = true;
        hostResetRasterWritten();
        hostDirectBegin();
        return;
    }
#endif
    if (raster_has_stale_pixels) {
        hostBorderUnwrittenPixels(internal.palette[0]);
        raster_has_stale_pixels = false;
    }
    hostPresentFrame(fb, render_fb);
    display_push_frame();
    hostResetRasterWritten();
#if OMEGA_ENABLE_HDMI
    hostDirectBegin();
#endif
}
