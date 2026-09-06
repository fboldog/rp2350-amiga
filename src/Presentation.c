#include "Presentation.h"

#include "Host.h"
#include "../omega/Chipset.h"
#include "../omega/DisplayLayout.h"
#include <string.h>

enum {
    EARLY_DISPLAY_LINE = 64,
    WRAPPED_PREFIX_PIXELS = 48,
    OCS_VERTICAL_BANK_LINES = 256,
};

static int isFullWidthDisplay(int diw_start) {
    return omegaDdfIsFullWidth(chipset.ddfstrt) &&
           diw_start < EARLY_DISPLAY_LINE;
}

static int needsWrappedPrefixRepair(int diw_start, int diw_stop) {
    return diw_stop < diw_start ||
           (OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL &&
            diw_stop + OCS_VERTICAL_BANK_LINES <= OMEGA_VIDEO_FRAME_LINES);
}

void hostPresentFrame(uint32_t *fb, uint32_t *render_fb) {
    const uint32_t border = internal.palette[0];
    const int diw_start = chipset.diwstrt >> 8;
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
    if (isFullWidthDisplay(diw_start)) {
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
    int diw_stop = chipset.diwstop >> 8;
    int repaired_wrapped_prefix =
        needsWrappedPrefixRepair(diw_start, diw_stop);
    for (int sy = 0; repaired_wrapped_prefix && sy + 1 < HOST_RASTER_H; ++sy) {
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
    if (repaired_wrapped_prefix) {
        for (int sy = 0; sy < HOST_RASTER_H; ++sy) {
            int dy = HOST_CONTENT_Y + sy * 2 - viewport_y_offset;
            if (dy < 0 || dy + 1 >= SCREEN_H)
                continue;
            uint32_t *row = &fb[dy * SCREEN_W];
            for (int x = HOST_VISIBLE_X0;
                 x < HOST_VISIBLE_X0 + WRAPPED_PREFIX_PIXELS; ++x)
                row[x] = border;
            memcpy(row + SCREEN_W, row, SCREEN_W * sizeof(uint32_t));
        }
    }

    // Map 32 Amiga HIRES beam pixels to 27 square host pixels.  Wrapped
    // scanlines have already moved their fetch prefix to the right edge, so
    // their presentation origin differs from an ordinary display window.
    int x_offset = repaired_wrapped_prefix
                 ? HOST_WRAP_X_OFFSET : HOST_NORMAL_X_OFFSET;
    if (x_offset == HOST_WRAP_X_OFFSET)
        x_offset = OMEGA_VIDEO_WRAP_X_OFFSET;
    uint32_t *scaled = render_fb;
    int first_content_y = HOST_CONTENT_Y - viewport_y_offset;
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

frame_ready:
    for (int i = 0; i < HOST_RASTER_PIXELS; ++i)
        render_fb[i] = border;
}
