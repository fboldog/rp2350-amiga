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
#include "hardware/sync.h"
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
// Direct RGB332 rendering: for full-width and plain narrow fetch layouts,
// converted pixels go straight into the free SRAM scanout frame, so the ARGB
// raster in PSRAM is neither written nor read.
//
// The conversion runs on core 1. Core 0 (the emulator) only enqueues each
// fetched bitplane block, palette updates and frame begin/end into a
// single-producer/single-consumer ring of 32-bit words; core 1 drains it in
// order between its HSTX line interrupts, so Copper palette changes still hit
// the right pixels. Core 1 owns the scanout frame, the row tracking and its
// palette copy.
#define DIRECT_MAX_ROWS 256
static bool direct_frame;         // this frame renders straight to RGB332
int hostDirectActive;             // mirrors direct_frame for DMA.c
static uint32_t direct_block[32]; // one planar block of ARGB pixels (HAM)

enum {
    MSG_BEGIN = 1,   // + 2 words: frame layout
    MSG_HIRES,       // + 2 words: planes 1..4
    MSG_LORES,       // + 3 words: planes 1..6
    MSG_PIXELS,      // + count / 4 words: ready RGB332 pixels (HAM)
    MSG_PALETTE,     // + 16 words: palette332[64]
    MSG_END,         // + 1 word: border colour (RGB332)
};
#define MSG_HEADER(type, row, x, extra) \
    ((uint32_t)(type) | (uint32_t)(row) << 4 | (uint32_t)(x) << 13 | \
     (uint32_t)(extra) << 23)

// Power of two. Core 1 waits up to one display frame for a pending frame to
// be shown while core 0 keeps queueing; a bigger ring skips fewer frames
// (idle Workbench shown/emulated: NTSC 22/32 fps with 8 KB, 27/32 with
// 32 KB). NTSC has ~32 KB of SRAM spare, PAL (larger frames) ~12 KB.
#if OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL
#define RING_WORDS 4096u  // 16 KB
#else
#define RING_WORDS 8192u  // 32 KB
#endif
static uint32_t ring[RING_WORDS];
static volatile uint32_t ring_head; // words produced (core 0)
static volatile uint32_t ring_tail; // words consumed (core 1)
static uint32_t palette_generation_sent = ~0u;

// ── Core 0: producer ──────────────────────────────────────────────────────
static inline void ringPush(const uint32_t *words, uint32_t count) {
    uint32_t head = ring_head;
    // Wait for room: core 1 converts faster than the emulator fetches, so
    // this only waits after a burst.
    while (head - __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) + count >
           RING_WORDS)
        tight_loop_contents();
    for (uint32_t i = 0; i < count; ++i)
        ring[(head + i) & (RING_WORDS - 1u)] = words[i];
    __atomic_store_n(&ring_head, head + count, __ATOMIC_RELEASE);
    __sev(); // wake core 1 from __wfe()
}

static inline void hostDirectSyncPalette(void) {
    if (internal.paletteGeneration == palette_generation_sent)
        return;
    palette_generation_sent = internal.paletteGeneration;
    uint32_t msg[17];
    msg[0] = MSG_HEADER(MSG_PALETTE, 0, 0, 0);
    memcpy(&msg[1], internal.palette332, sizeof(internal.palette332));
    ringPush(msg, count_of(msg));
}

static void hostDirectBegin(void) {
    const bool full_width = omegaDdfIsFullWidth(chipset.ddfstrt);
    // Narrow layouts that need hostPresentFrame()'s wrapped-fetch
    // reconstruction (pixels moved between lines) keep the ARGB path.
    const bool narrow = !full_width &&
        !omegaDiwCrossesVerticalBank(chipset.diwstrt, chipset.diwstop);
    direct_frame = full_width || narrow;
    hostDirectActive = direct_frame;
    if (!direct_frame)
        return;
    int step, first_row, rotation;
    if (narrow) {
        // hostPresentFrame(): raster row sy lands on framebuffer line
        // HOST_CONTENT_Y + 2 * sy - viewport_y_offset, and columns
        // [HOST_VISIBLE_X0, HOST_VISIBLE_X1) come from raster column
        // x + HOST_FETCH_LEAD.
        const int viewport_y_offset =
            (OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL &&
             omegaDiwVerticalStart(chipset.diwstrt) < 64)
            ? HOST_CONTENT_Y : OMEGA_VIDEO_VIEWPORT_Y_OFFSET;
        step = 1;
        first_row = (HOST_CONTENT_Y - viewport_y_offset) / 2;
        rotation = 0;
    } else {
        const int alternate_rows =
            host.displayIsLores &&
            omegaLoresUsesAlternateRasterRows(chipset.diwstrt,
                                               chipset.diwstop);
        step = alternate_rows ? 2 : 1;
        // dvi_display_submit_raster() skips y_offset / 2 image rows (64 / 2).
        first_row = host.displayIsLores && !alternate_rows ? 32 : 0;
        rotation = omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop);
    }
    int height = dvi_display_direct_height();
    if (height > DIRECT_MAX_ROWS)
        height = DIRECT_MAX_ROWS;
    const uint32_t msg[3] = {
        MSG_HEADER(MSG_BEGIN, 0, 0, 0),
        (uint16_t)first_row | (uint32_t)step << 16 | (uint32_t)narrow << 24,
        (uint32_t)rotation | (uint32_t)height << 16,
    };
    ringPush(msg, count_of(msg));
}

void hostDirectHires(int row, int x, uint16_t p1, uint16_t p2,
                     uint16_t p3, uint16_t p4) {
    hostDirectSyncPalette();
    const uint32_t msg[3] = {
        MSG_HEADER(MSG_HIRES, row, x, 0),
        p1 | (uint32_t)p2 << 16,
        p3 | (uint32_t)p4 << 16,
    };
    ringPush(msg, count_of(msg));
}

void hostDirectLores(int row, int x, uint16_t p1, uint16_t p2, uint16_t p3,
                     uint16_t p4, uint16_t p5, uint16_t p6) {
    hostDirectSyncPalette();
    const uint32_t msg[4] = {
        MSG_HEADER(MSG_LORES, row, x, 0),
        p1 | (uint32_t)p2 << 16,
        p3 | (uint32_t)p4 << 16,
        p5 | (uint32_t)p6 << 16,
    };
    ringPush(msg, count_of(msg));
}

// ARGB block (HAM path, converted here because HAM depends on the previous
// pixel) -> RGB332 pixels for core 1.
static void hostDirectCommit(int row, int x, int width) {
    uint32_t msg[1 + 32 / 4];
    msg[0] = MSG_HEADER(MSG_PIXELS, row, x, width);
    uint8_t *pixels = (uint8_t *)&msg[1];
    for (int i = 0; i < width; ++i)
        pixels[i] = dvi_rgb332(direct_block[i]);
    ringPush(msg, 1u + ((uint32_t)width + 3u) / 4u);
}

static void hostDirectFinish(void) {
    const uint32_t msg[2] = {
        MSG_HEADER(MSG_END, 0, 0, 0),
        dvi_rgb332(internal.palette[0]),
    };
    ringPush(msg, count_of(msg));
    static bool first_frame_queued;
    if (!first_frame_queued) {
        first_frame_queued = true;
        printf("DVI: first direct RGB332 emulator frame queued\n");
    }
}

// ── Core 1: consumer ──────────────────────────────────────────────────────
static uint8_t *c1_image;          // acquired scanout frame, or NULL
static int c1_first_row;           // image row of raster row 0
static int c1_step;                // raster rows per image row
static int c1_rotation;            // DDF row rotation in raster columns
static bool c1_narrow;             // narrow fetch layout
static int c1_height;              // image rows
// Core 1's loops must not become libc memset/memcpy calls (flash code).
#define C1_FUNC(name) \
    __attribute__((optimize("no-tree-loop-distribute-patterns"))) \
    __not_in_flash_func(name)
static int16_t c1_row_min[DIRECT_MAX_ROWS];
static int16_t c1_row_max[DIRECT_MAX_ROWS];
static uint8_t c1_palette[64];

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

static inline void C1_FUNC(c1Copy)(uint8_t *line,
                                               const uint8_t *pixels,
                                               int y, int x, int count) {
    for (int i = 0; i < count; ++i)
        line[x + i] = pixels[i];
    if (x < c1_row_min[y]) c1_row_min[y] = (int16_t)x;
    if (x + count > c1_row_max[y]) c1_row_max[y] = (int16_t)(x + count);
}

// Places `width` RGB332 pixels of raster (row, x) into the scanout frame.
static void C1_FUNC(c1Place)(int row, int x,
                                         const uint8_t *pixels, int width) {
    if (!c1_image || row % c1_step)
        return;
    const int y = c1_first_row + row / c1_step;
    if (y < 0 || y >= c1_height)
        return;
    uint8_t *line = c1_image + y * DVI_DIRECT_IMAGE_WIDTH;
    if (c1_narrow) {
        int image_x = x - HOST_FETCH_LEAD;
        int first = 0, count = width;
        if (image_x < HOST_VISIBLE_X0) {
            first = HOST_VISIBLE_X0 - image_x;
            image_x = HOST_VISIBLE_X0;
        }
        if (image_x + (count - first) > HOST_VISIBLE_X1)
            count = first + HOST_VISIBLE_X1 - image_x;
        if (count > first)
            c1Copy(line, pixels + first, y, image_x, count - first);
        return;
    }
    int image_x = x - c1_rotation;
    if (image_x < 0)
        image_x += HOST_RASTER_W;
    int first = HOST_RASTER_W - image_x;
    if (first > width)
        first = width;
    c1Copy(line, pixels, y, image_x, first);
    if (first < width) // the block wraps around the rotated row
        c1Copy(line, pixels + first, y, 0, width - first);
}

static void C1_FUNC(c1Hires)(int row, int x, uint32_t p12,
                                         uint32_t p34) {
    const uint16_t p1 = (uint16_t)p12, p2 = (uint16_t)(p12 >> 16);
    const uint16_t p3 = (uint16_t)p34, p4 = (uint16_t)(p34 >> 16);
    uint8_t pixels[16];
    for (int half = 0; half < 2; ++half) {
        const int shift = half * 8;
        for (int part = 0; part < 2; ++part) {
            uint32_t index = C2P(p1, shift, part) |
                             C2P(p2, shift, part) << 1 |
                             C2P(p3, shift, part) << 2 |
                             C2P(p4, shift, part) << 3;
            uint8_t *out = pixels + half * 8 + part * 4;
            out[0] = c1_palette[index & 0xffu];
            out[1] = c1_palette[(index >> 8) & 0xffu];
            out[2] = c1_palette[(index >> 16) & 0xffu];
            out[3] = c1_palette[index >> 24];
        }
    }
    c1Place(row, x, pixels, 16);
}

// LORES: 16 pixels of up to 6 planes (EHB palette 32..63), each doubled.
static void C1_FUNC(c1Lores)(int row, int x, uint32_t p12,
                                         uint32_t p34, uint32_t p56) {
    const uint16_t p1 = (uint16_t)p12, p2 = (uint16_t)(p12 >> 16);
    const uint16_t p3 = (uint16_t)p34, p4 = (uint16_t)(p34 >> 16);
    const uint16_t p5 = (uint16_t)p56, p6 = (uint16_t)(p56 >> 16);
    uint8_t pixels[32];
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
                const uint8_t colour = c1_palette[(index >> (8 * k)) & 0xffu];
                out[2 * k] = colour;
                out[2 * k + 1] = colour;
            }
        }
    }
    c1Place(row, x, pixels, 32);
}

static void C1_FUNC(c1Begin)(uint32_t layout, uint32_t size) {
    c1_first_row = (int16_t)(layout & 0xffffu);
    c1_step = (int)((layout >> 16) & 0xffu);
    c1_narrow = (layout >> 24) & 1u;
    c1_rotation = (int)(size & 0xffffu);
    c1_height = (int)(size >> 16);
    for (int y = 0; y < DIRECT_MAX_ROWS; ++y) {
        c1_row_min[y] = DVI_DIRECT_IMAGE_WIDTH;
        c1_row_max[y] = 0;
    }
    // The previous frame may still wait for the next DVI frame boundary
    // (at most one display frame). Wait for the swap while core 0 keeps
    // queueing; if the ring gets close to full first, skip this frame so
    // the emulator never stalls. A finished frame is never discarded.
    while (dvi_display_frame_pending() &&
           ring_head - ring_tail < RING_WORDS * 3u / 4u)
        tight_loop_contents();
    c1_image = dvi_display_frame_pending()
             ? NULL
             : dvi_display_direct_acquire(); // NULL while the boot pattern
}

static void C1_FUNC(c1End)(uint8_t border) {
    if (!c1_image)
        return;
    for (int y = 0; y < c1_height; ++y) {
        uint8_t *line = c1_image + y * DVI_DIRECT_IMAGE_WIDTH;
        int written_min = c1_row_min[y];
        int written_max = c1_row_max[y];
        if (written_max <= written_min)
            written_min = written_max = DVI_DIRECT_IMAGE_WIDTH;
        // Plain loops: libc memset executes from flash.
        for (int x = 0; x < written_min; ++x)
            line[x] = border;
        for (int x = written_max; x < DVI_DIRECT_IMAGE_WIDTH; ++x)
            line[x] = border;
    }
    dvi_display_direct_publish_rgb332(border);
    c1_image = NULL;
}

static inline uint32_t c1Word(uint32_t index) {
    return ring[index & (RING_WORDS - 1u)];
}

void C1_FUNC(hostCore1Loop)(void) {
    uint32_t tail = ring_tail;
    for (;;) {
        uint32_t head;
        while ((head = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE)) == tail)
            __wfe();
        while (tail != head) {
            const uint32_t header = c1Word(tail);
            const int type = (int)(header & 0xfu);
            const int row = (int)((header >> 4) & 0x1ffu);
            const int x = (int)((header >> 13) & 0x3ffu);
            uint32_t length;
            switch (type) {
            case MSG_HIRES:
                c1Hires(row, x, c1Word(tail + 1u), c1Word(tail + 2u));
                length = 3;
                break;
            case MSG_LORES:
                c1Lores(row, x, c1Word(tail + 1u), c1Word(tail + 2u),
                        c1Word(tail + 3u));
                length = 4;
                break;
            case MSG_PIXELS: {
                const int count = (int)(header >> 23);
                uint32_t words[32 / 4];
                length = 1u + ((uint32_t)count + 3u) / 4u;
                for (uint32_t i = 1; i < length; ++i)
                    words[i - 1u] = c1Word(tail + i);
                c1Place(row, x, (const uint8_t *)words, count);
                break;
            }
            case MSG_PALETTE: {
                uint32_t words[16];
                for (uint32_t i = 0; i < 16u; ++i)
                    words[i] = c1Word(tail + 1u + i);
                for (uint32_t i = 0; i < 16u; ++i) {
                    c1_palette[4 * i] = (uint8_t)words[i];
                    c1_palette[4 * i + 1] = (uint8_t)(words[i] >> 8);
                    c1_palette[4 * i + 2] = (uint8_t)(words[i] >> 16);
                    c1_palette[4 * i + 3] = (uint8_t)(words[i] >> 24);
                }
                length = 17;
                break;
            }
            case MSG_BEGIN:
                c1Begin(c1Word(tail + 1u), c1Word(tail + 2u));
                length = 3;
                break;
            case MSG_END:
                c1End((uint8_t)c1Word(tail + 1u));
                length = 2;
                break;
            default:
                panic("host: bad core-1 ring message %08lx",
                      (unsigned long)header);
            }
            tail += length;
            __atomic_store_n(&ring_tail, tail, __ATOMIC_RELEASE);
        }
    }
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
    // The ARGB paths below write scanout frames from core 0; let core 1
    // finish converting the previous direct frame first.
    while (__atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) != ring_head)
        tight_loop_contents();
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
