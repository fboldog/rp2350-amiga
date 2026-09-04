// SDL-free desktop host layer for running the Omega core natively (no RP2350,
// no PSRAM, no display hardware).  Renders planar->chunky into a plain malloc'd
// framebuffer so the emulator core can be exercised head-less on a PC.
//
// The planar2chunky routines are byte-for-byte the same as src/Host.c (they are
// platform independent); only hostInit / hostDisplay differ.

#include "Host.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/CPU.h"
#include "../omega/VideoStandard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2
#include "display_sdl.h"
#endif

Host_t host;

static uint32_t *fb;                    // presented SCREEN_W * SCREEN_H image
static uint32_t *render_fb;             // intermediate DMA beam raster
unsigned long native_frame_counter = 0; // bumped by hostDisplay()

// ── Amiga key-code table (unused head-less, kept for link compatibility) ──
static const uint8_t keyMapping[] = { 0 };

void pressKey(uint16_t keyCode)   { (void)keyCode; }
void releaseKey(uint16_t keyCode) { (void)keyCode; }
void toggleLEDs(void)             { }

// ── Lifecycle ────────────────────────────────────────────────────────────
void hostInit(void) {
    fb = calloc(SCREEN_W * SCREEN_H, sizeof(uint32_t));
    render_fb = calloc(HOST_RASTER_PIXELS, sizeof(uint32_t));
    if (!fb || !render_fb) {
        fprintf(stderr, "Host init: framebuffer allocation failed\n");
        exit(1);
    }
    host.pixels    = render_fb;
    host.FBCounter = 0;
    host.vblCount  = 0;
    printf("Host init (native): framebuffer %p (%dx%d ARGB)\n",
           (void *)fb, SCREEN_W, SCREEN_H);
}

void hostDisplay(void) {
    const uint32_t border = internal.palette[0];

    // Convert the raw DMA fetch raster to the visible display.  The first
    // fetch words are pipeline/overscan data and must be clipped, not wrapped
    // around to the opposite edge.
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i)
        fb[i] = border;
    for (int sy = 0; sy < HOST_RASTER_H; ++sy) {
        int dy = HOST_CONTENT_Y + sy * 2 - OMEGA_VIDEO_VIEWPORT_Y_OFFSET;
        if (dy < 0 || dy + 1 >= SCREEN_H)
            continue;
        for (int x = HOST_VISIBLE_X0; x < HOST_VISIBLE_X1; ++x) {
            int sx = HOST_FETCH_LEAD + x;
            uint32_t pixel = render_fb[sy * HOST_RASTER_W + sx];
            fb[dy * SCREEN_W + x] = pixel;
        }

        memcpy(&fb[(dy + 1) * SCREEN_W], &fb[dy * SCREEN_W],
               SCREEN_W * sizeof(uint32_t));
    }

    // The fetch pipeline places the next logical scanline's prefix after the
    // current line.  Rejoin such prefixes to a right-edge object, then remove
    // them from the following line's left edge.
    for (int sy = 0;
         (chipset.diwstop >> 8) < (chipset.diwstrt >> 8) &&
         sy + 1 < HOST_RASTER_H;
         ++sy) {
        int dy = HOST_CONTENT_Y + sy * 2 - OMEGA_VIDEO_VIEWPORT_Y_OFFSET;
        if (dy < 0 || dy + 3 >= SCREEN_H)
            continue;
        uint32_t *row = &fb[dy * SCREEN_W];
        uint32_t *next = row + SCREEN_W * 2;
        int right = HOST_VISIBLE_X1 - 1;
        while (right >= SCREEN_W / 2 && row[right] == border) right--;
        int prefix_start = HOST_VISIBLE_X0;
        while (prefix_start < HOST_VISIBLE_X0 + 48 &&
               next[prefix_start] == border)
            prefix_start++;
        if (right < SCREEN_W / 2 || prefix_start == HOST_VISIBLE_X0 + 48)
            continue;

        int prefix_end = prefix_start;
        while (prefix_end < HOST_VISIBLE_X0 + 48 && next[prefix_end] != border)
            prefix_end++;
        int count = prefix_end - HOST_VISIBLE_X0;
        if (count > SCREEN_W - right - 1) count = SCREEN_W - right - 1;
        for (int x = 0; x < count; ++x) {
            row[right + 1 + x] = next[HOST_VISIBLE_X0 + x];
            next[HOST_VISIBLE_X0 + x] = border;
        }
        memcpy(row + SCREEN_W, row, SCREEN_W * sizeof(uint32_t));
        memcpy(next + SCREEN_W, next, SCREEN_W * sizeof(uint32_t));
    }
    if ((chipset.diwstop >> 8) < (chipset.diwstrt >> 8)) {
        for (int sy = 0; sy < HOST_RASTER_H; ++sy) {
            int dy = HOST_CONTENT_Y + sy * 2 - OMEGA_VIDEO_VIEWPORT_Y_OFFSET;
            if (dy < 0 || dy + 1 >= SCREEN_H)
                continue;
            uint32_t *row = &fb[dy * SCREEN_W];
            for (int x = HOST_VISIBLE_X0; x < HOST_VISIBLE_X0 + 48; ++x)
                row[x] = border;
            memcpy(row + SCREEN_W, row, SCREEN_W * sizeof(uint32_t));
        }
    }

    // Map 32 Amiga HIRES beam pixels to 27 square host pixels.  Wrapped
    // scanlines have already moved their fetch prefix to the right edge, so
    // their presentation origin differs from an ordinary display window.
    int x_offset = ((chipset.diwstop >> 8) < (chipset.diwstrt >> 8))
                 ? HOST_WRAP_X_OFFSET : HOST_NORMAL_X_OFFSET;
    uint32_t *scaled = render_fb;
    int first_content_y = HOST_CONTENT_Y - OMEGA_VIDEO_VIEWPORT_Y_OFFSET;
    if (first_content_y < 0) first_content_y = 0;
    for (int y = first_content_y; y < SCREEN_H; ++y) {
        uint32_t *row = &fb[y * SCREEN_W];
        for (int x = 0; x < SCREEN_W; ++x) scaled[x] = border;
        int scaled_w = SCREEN_W * HOST_ASPECT_X_NUM / HOST_ASPECT_X_DEN;
        for (int dx = 0; dx < scaled_w; ++dx) {
            int sx = dx * HOST_ASPECT_X_DEN / HOST_ASPECT_X_NUM;
            scaled[x_offset + dx] = row[sx];
        }
        memcpy(row, scaled, SCREEN_W * sizeof(uint32_t));
    }

    for (int i = 0; i < HOST_RASTER_PIXELS; ++i)
        render_fb[i] = border;
    native_frame_counter++;
#ifdef HAVE_SDL2
    sdl_display_push(fb);
#endif
}

// Dump the current framebuffer to a binary PPM (P6).  Called from main.
void native_dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i) {
        uint32_t p = fb[i];                 // ARGB (0x00RRGGBB from palette)
        unsigned char rgb[3] = {
            (unsigned char)((p >> 16) & 0xFF),
            (unsigned char)((p >>  8) & 0xFF),
            (unsigned char)( p        & 0xFF),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// Count non-black pixels – a cheap "is anything being drawn?" probe.
unsigned long native_nonblack_pixels(void) {
    unsigned long n = 0;
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i)
        if ((fb[i] & 0x00FFFFFF) != 0) ++n;
    return n;
}

// ── Planar -> Chunky pixel conversion (identical to src/Host.c) ──────────

void hiresPlanar2Chunky(uint32_t *pixBuff, uint32_t *palette,
                        uint16_t plane1, uint16_t plane2,
                        uint16_t plane3, uint16_t plane4) {
    (void)palette;
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
    (void)palette;
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
    (void)palette;
    int counter = host.FBCounter;

    for (int j = 7; j > -1; --j) {
        uint32_t c1 =  (plane1 >> j) & 1;
        c1 |= (((plane2 >> j) & 1) << 1);
        c1 |= (((plane3 >> j) & 1) << 2);
        c1 |= (((plane4 >> j) & 1) << 3);
        c1 |= (((plane5 >> j) & 1) << 4);
        c1 |= (((plane6 >> j) & 1) << 5);

        if (c1 & 0xF0) {
            int      ctrl = c1 >> 4;
            uint32_t col  = c1 & 0xF;
            col = (col << 4) | col;
            uint32_t prev = (counter > 0) ? pixBuff[counter - 1] : 0;
            switch (ctrl) {
                case 1: prev = (prev & 0xFFFFFF00u) |  col;        break;
                case 2: prev = (prev & 0xFF00FFFFu) | (col << 16); break;
                case 3: prev = (prev & 0xFFFF00FFu) | (col <<  8); break;
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
            int      ctrl = c2 >> 4;
            uint32_t col  = c2 & 0xF;
            col = (col << 4) | col;
            uint32_t prev = pixBuff[counter + 15];
            switch (ctrl) {
                case 1: prev = (prev & 0xFFFFFF00u) |  col;        break;
                case 2: prev = (prev & 0xFF00FFFFu) | (col << 16); break;
                case 3: prev = (prev & 0xFFFF00FFu) | (col <<  8); break;
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
