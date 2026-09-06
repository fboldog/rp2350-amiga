#include "Presentation.h"

#include "Host.h"
#include "../omega/Chipset.h"
#include "../omega/DisplayLayout.h"
#include <string.h>

enum {
    EARLY_DISPLAY_LINE = 64,
    WRAPPED_PREFIX_PIXELS = 48,
};

static int isFullWidthDisplay(void) {
    return omegaDdfIsFullWidth(chipset.ddfstrt);
}

void hostPresentFrame(uint32_t *fb, uint32_t *render_fb) {
    const uint32_t border = internal.palette[0];
    const int diw_start = omegaDiwVerticalStart(chipset.diwstrt);
    const int viewport_y_offset =
        (OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL &&
         diw_start < EARLY_DISPLAY_LINE)
        ? HOST_CONTENT_Y : OMEGA_VIDEO_VIEWPORT_Y_OFFSET;

    // Convert the raw DMA fetch raster to the visible display.  The first
    // fetch words are pipeline/overscan data and must be clipped, not wrapped
    // around to the opposite edge.
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i)
        fb[i] = border;

    // A full-width HIRES fetch is stored by the DMA renderer as 320 packed
    // samples per scanline. Present each sample twice horizontally and each
    // scanline twice vertically. Narrow wrapped fetch windows use the pipeline
    // reconstruction path below.
    if (isFullWidthDisplay()) {
        int row_rotation =
            omegaDdfRowRotation(chipset.ddfstrt, chipset.ddfstop);
        // This full-width LORES layout already advances two beam rows for
        // each logical picture row.  Sample those rows once before the host
        // performs its normal 2x vertical integer scaling.
        int source_step = host.displayIsLores ? 2 : 1;
        int rows = HOST_RASTER_H / source_step;
        if (rows > SCREEN_H / 2) rows = SCREEN_H / 2;
        for (int y = 0; y < rows; ++y) {
            uint32_t *dst = &fb[(y * 2) * SCREEN_W];
            const uint32_t *src = &render_fb[(y * source_step) * SCREEN_W];
            if (row_rotation) {
                for (int x = 0; x < SCREEN_W; ++x)
                    dst[x] = src[(x + row_rotation) % SCREEN_W];
            } else {
                memcpy(dst, src, SCREEN_W * sizeof(uint32_t));
            }
            memcpy(dst + SCREEN_W, dst, SCREEN_W * sizeof(uint32_t));
        }
        goto frame_ready;
    }

    for (int sy = 0; sy < HOST_RASTER_H; ++sy) {
        int dy = HOST_CONTENT_Y + sy * 2 - viewport_y_offset;
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
    int reconstruct_wrapped_fetch_rows =
        omegaDiwCrossesVerticalBank(chipset.diwstrt, chipset.diwstop);
    for (int sy = 0;
         reconstruct_wrapped_fetch_rows && sy + 1 < HOST_RASTER_H; ++sy) {
        int dy = HOST_CONTENT_Y + sy * 2 - viewport_y_offset;
        if (dy < 0 || dy + 3 >= SCREEN_H)
            continue;
        uint32_t *row = &fb[dy * SCREEN_W];
        uint32_t *next = row + SCREEN_W * 2;
        int right = HOST_VISIBLE_X1 - 1;
        while (right >= SCREEN_W / 2 && row[right] == border) right--;
        int prefix_start = HOST_VISIBLE_X0;
        while (prefix_start < HOST_VISIBLE_X0 + WRAPPED_PREFIX_PIXELS &&
               next[prefix_start] == border)
            prefix_start++;
        if (right < SCREEN_W / 2 ||
            prefix_start == HOST_VISIBLE_X0 + WRAPPED_PREFIX_PIXELS)
            continue;

        int prefix_end = prefix_start;
        while (prefix_end < HOST_VISIBLE_X0 + WRAPPED_PREFIX_PIXELS &&
               next[prefix_end] != border)
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
    if (reconstruct_wrapped_fetch_rows) {
        for (int sy = 0; sy < HOST_RASTER_H; ++sy) {
            int dy = HOST_CONTENT_Y + sy * 2 - viewport_y_offset;
            if (dy < 0 || dy + 1 >= SCREEN_H)
                continue;
            uint32_t *row = &fb[dy * SCREEN_W];
            for (int x = HOST_VISIBLE_X0;
                 x < HOST_VISIBLE_X0 + WRAPPED_PREFIX_PIXELS; ++x)
                row[x] = border;
            // Prefix reconstruction uses raw fetch coordinates. Remove that
            // raster's left origin to keep the complete logical row visible.
            memmove(row, row + HOST_VISIBLE_X0,
                    (SCREEN_W - HOST_VISIBLE_X0) * sizeof(uint32_t));
            for (int x = SCREEN_W - HOST_VISIBLE_X0; x < SCREEN_W; ++x)
                row[x] = border;
            memcpy(row + SCREEN_W, row, SCREEN_W * sizeof(uint32_t));
        }
    }

frame_ready:
    for (int i = 0; i < HOST_RASTER_PIXELS; ++i)
        render_fb[i] = border;
}
