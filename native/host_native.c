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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2
#include "display_sdl.h"
#endif

Host_t host;

static uint32_t *fb;                    // SCREEN_W * SCREEN_H, 32-bit ARGB
unsigned long native_frame_counter = 0; // bumped by hostDisplay()

// ── Amiga key-code table (unused head-less, kept for link compatibility) ──
static const uint8_t keyMapping[] = { 0 };

void pressKey(uint16_t keyCode)   { (void)keyCode; }
void releaseKey(uint16_t keyCode) { (void)keyCode; }
void toggleLEDs(void)             { }

// ── Lifecycle ────────────────────────────────────────────────────────────
void hostInit(void) {
    fb = calloc(SCREEN_W * SCREEN_H, sizeof(uint32_t));
    host.pixels    = fb;
    host.FBCounter = 0;
    host.vblCount  = 0;
    printf("Host init (native): framebuffer %p (%dx%d ARGB)\n",
           (void *)fb, SCREEN_W, SCREEN_H);
}

void hostDisplay(void) {
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
