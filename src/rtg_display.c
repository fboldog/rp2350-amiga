// RTG on the HDMI output: the Picasso II+'s picture (omega/Picasso2.c).
// Runs on core 1 from flash (Host.c's code is in SRAM, which is full).
//
#include "rtg_display.h"
#include "dvi_display.h"
#include "Host.h"
#include "psram.h"
#include "../omega/Picasso2.h"
#include "hardware/regs/addressmap.h"
#include <stdbool.h>
#include <stdint.h>

// While the card's monitor switch is on, core 1 draws no Amiga frames: at
// each frame it copies the VRAM pages of the visible screen that changed
// (rtg_dirty, marked by MemoryBoot.c and the blitter) into the scanout's
// frame memory and refreshes the palette and the cursor. 8-bit screens
// keep their palette; 15/16/24-bit ones are shown as RGB332.
extern uint32_t rtg_dirty[];
#define RTG_PAGES (BOARD_MAP_RTG_VRAM_SIZE >> 12)
#define RTG_VRAM ((const uint8_t *)(PSRAM_BASE - XIP_BASE + \
                                    XIP_NOCACHE_NOALLOC_BASE + \
                                    BOARD_MAP_RTG_VRAM_OFFSET))
static bool rtg_was_on;
static P2Display rtg_shown;

static inline uint8_t rtg_rgb332(uint32_t rgb) {
    return (uint8_t)((rgb >> 16 & 0xe0u) | (rgb >> 11 & 0x1cu) |
                     (rgb >> 6 & 0x03u));
}

// Pixels [x0, x1) of row r from VRAM bytes at `a`, read uncached (the
// copy would otherwise flush the emulator's code out of the XIP cache) as
// aligned words, a chunk at a time.
static void rtg_copy(uint8_t *frame, const P2Display *d, int r, int x0,
                     int x1, uint32_t a) {
    uint32_t words[64 + 2];
    const int bpp = d->bytesPerPixel;
    const int chunk = (int)(sizeof words - 8u) / bpp;
    while (x1 - x0 > chunk) {
        rtg_copy(frame, d, r, x0, x0 + chunk, a);
        x0 += chunk;
        a += (uint32_t)(chunk * bpp);
    }
    const uint32_t first = a & ~3u;
    const uint32_t last = a + (uint32_t)((x1 - x0) * bpp);
    for (uint32_t w = first, i = 0; w < last; w += 4u, ++i)
        words[i] = *(const volatile uint32_t *)
                   &RTG_VRAM[w & (BOARD_MAP_RTG_VRAM_SIZE - 1)];
    const uint8_t *bytes = (const uint8_t *)words + (a - first);
    uint8_t *dst = frame + r * d->width + x0;
    for (int x = x0; x < x1; ++x, bytes += bpp) {
        const uint8_t *s = bytes;
        if (bpp == 1) {
            *dst++ = s[0];
        } else if (d->bytesPerPixel == 2) {
            const unsigned p = s[0] | s[1] << 8;
            const uint32_t rgb = d->rgb565
                ? (p >> 11 & 0x1fu) << 19 | (p >> 5 & 0x3fu) << 10 | (p & 0x1fu) << 3
                : (p >> 10 & 0x1fu) << 19 | (p >> 5 & 0x1fu) << 11 | (p & 0x1fu) << 3;
            *dst++ = rtg_rgb332(rgb);
        } else {
            *dst++ = rtg_rgb332((uint32_t)s[2] << 16 | (uint32_t)s[1] << 8 | s[0]);
        }
    }
}

// The rows of the visible screen in VRAM page p.
static void rtg_copy_page(uint8_t *frame, const P2Display *d, uint32_t p) {
    const uint32_t ps = p << 12, pe = ps + 4096u;
    const uint32_t row_bytes = (uint32_t)d->width * (uint32_t)d->bytesPerPixel;
    const uint32_t end = d->start + (uint32_t)(d->height - 1) * d->pitch + row_bytes;
    if (pe <= d->start || ps >= end)
        return;
    int r = ps > d->start ? (int)((ps - d->start) / d->pitch) : 0;
    for (; r < d->height; ++r) {
        const uint32_t rs = d->start + (uint32_t)r * d->pitch;
        if (rs >= pe)
            break;
        const uint32_t a0 = rs > ps ? rs : ps;
        const uint32_t a1 = rs + row_bytes < pe ? rs + row_bytes : pe;
        if (a0 >= a1)
            continue;
        // whole pixels: one cut by the page edge is copied with both pages
        const int x0 = (int)((a0 - rs) / (uint32_t)d->bytesPerPixel);
        const int x1 = (int)((a1 - rs + (uint32_t)d->bytesPerPixel - 1) /
                             (uint32_t)d->bytesPerPixel);
        rtg_copy(frame, d, r, x0, x1 > d->width ? d->width : x1,
                 rs + (uint32_t)x0 * (uint32_t)d->bytesPerPixel);
    }
}

int rtgFrame(void) {
    if (!p2Showing()) {
        if (!rtg_was_on)
            return 0;
        rtg_was_on = false;
        dvi_display_rtg_off();
        return -1;
    }
    uint32_t capacity, *palette;
    uint8_t *frame = dvi_display_rtg_frame(&capacity, &palette);
    P2Display d;
    p2Display(&d);
    if (d.width > 640) d.width = 640;
    if (d.width < 8 || d.height < 1 || d.pitch == 0)
        return 1;
    if ((uint32_t)d.width * (uint32_t)d.height > capacity)
        d.height = (int)(capacity / (uint32_t)d.width);
    const bool full = !rtg_was_on || d.width != rtg_shown.width ||
                      d.height != rtg_shown.height ||
                      d.start != rtg_shown.start || d.pitch != rtg_shown.pitch ||
                      d.bytesPerPixel != rtg_shown.bytesPerPixel ||
                      d.rgb565 != rtg_shown.rgb565;
    rtg_shown = d;
    for (uint32_t w = 0; w < RTG_PAGES / 32u; ++w) {
        uint32_t bits = __atomic_exchange_n(&rtg_dirty[w], 0u, __ATOMIC_ACQ_REL);
        if (full)
            bits = ~0u;
        while (bits) {
            const uint32_t b = (uint32_t)__builtin_ctz(bits);
            bits &= bits - 1u;
            rtg_copy_page(frame, &d, w * 32u + b);
            hostAudioPoll();
        }
    }
    for (unsigned i = 0; i < 256u; ++i)
        palette[i] = d.bytesPerPixel == 1
            ? p2Rgb(i & d.pixelMask)
            : ((i >> 5) * 255u / 7u) << 16 | ((i >> 2 & 7u) * 255u / 7u) << 8 |
              (i & 3u) * 255u / 3u;
    // The cursor's planes, in SRAM after the picture when they fit.
    dvi_rtg_cursor_t c = {
        d.cursorOn, d.cursorSize, d.cursorX, d.cursorY,
        { p2Rgb(P2_CURSOR_RGB), p2Rgb(P2_CURSOR_RGB + 1) },
        &RTG_VRAM[d.cursorPattern & (BOARD_MAP_RTG_VRAM_SIZE - 1)] };
    const uint32_t used = (uint32_t)d.width * (uint32_t)d.height;
    if (d.cursorOn && capacity - used >= 1024u) {
        uint8_t *copy = frame + capacity - 1024u;
        for (unsigned i = 0; i < 1024u; i += 4u)
            *(uint32_t *)(copy + i) = *(const volatile uint32_t *)(c.pattern + i);
        c.pattern = copy;
    }
    dvi_display_rtg_show(d.width, d.height, &c);
    rtg_was_on = true;
    return 1;
}
