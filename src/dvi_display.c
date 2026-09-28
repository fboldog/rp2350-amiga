// PIO DVI bring-up for supported RP2350B boards.
//
// PicoDVI performs TMDS encoding on core 1 and serialises three data lanes
// with PIO. Core 0 converts completed emulator frames to double-buffered
// RGB scanout frames in cached PSRAM; core 1 changes buffers only at a DVI
// frame boundary.

#include "dvi_display.h"
#include "board_config.h"
#include "Host.h"
#include "psram.h"
#include "../omega/VideoStandard.h"

#include "dvi.h"
#include "tmds_encode.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/busctrl.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#if OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL
// CEA 720x576p50: 27 MHz pixel clock, 864x625 total pixels.
static const struct dvi_timing amiga_dvi_timing = {
    .h_sync_polarity = false,
    .h_front_porch = 12,
    .h_sync_width = 64,
    .h_back_porch = 68,
    .h_active_pixels = 720,
    .v_sync_polarity = false,
    .v_front_porch = 5,
    .v_sync_width = 5,
    .v_back_porch = 39,
    .v_active_lines = 576,
    .bit_clk_khz = 270000,
};
#define DVI_ACTIVE_WIDTH   720u
#define DVI_MODE_PREFIX    "720x576p50"
#define SOURCE_HEIGHT      288u
// PAL screens such as Workbench use 256 lines; showing only 200 cut off the
// lower edge of windows.
#define AMIGA_SOURCE_HEIGHT 256u
#else
// NTSC builds run clk_sys at 252 MHz, which is exactly VGA 640x480p60
// (25.2 MHz pixels). PicoDVI's 720x480p60 timing needs 270 MHz; at 252 MHz it
// produced a non-standard ~55.9 Hz mode that capture devices lock onto
// unreliably. 640 active pixels also match the Amiga image without borders.
#define DVI_ACTIVE_WIDTH   640u
#define DVI_MODE_PREFIX    "640x480p60"
#define SOURCE_HEIGHT      240u
#define AMIGA_SOURCE_HEIGHT 200u
#endif

#if BOARD_WEACT_STUDIO_RP2350B_CORE
#define DVI_FULL_WIDTH     1
#define SOURCE_WIDTH       DVI_ACTIVE_WIDTH
#define AMIGA_SOURCE_WIDTH 640u
#define DVI_MODE_NAME      DVI_MODE_PREFIX " full-width 640x400"
typedef uint8_t dvi_pixel_t;
#else
#define DVI_FULL_WIDTH     0
#define SOURCE_WIDTH       (DVI_ACTIVE_WIDTH / 2u)
#define AMIGA_SOURCE_WIDTH 320u
#define DVI_MODE_NAME      DVI_MODE_PREFIX " (640x400 image doubled)"
typedef uint8_t dvi_pixel_t;
#endif

#define DVI_IMAGE_X0 ((SOURCE_WIDTH - AMIGA_SOURCE_WIDTH) / 2u)
#define DVI_IMAGE_Y0 ((SOURCE_HEIGHT - AMIGA_SOURCE_HEIGHT) / 2u)

#if DVI_FULL_WIDTH
// Full-width scanlines are too large to fetch from PSRAM on core 1: the
// emulator saturates the shared QMI bus and 720-byte line reads regularly
// missed the scanline deadline (PicoDVI then emits red lines). Keep only the
// 640x200 (NTSC) or 640x256 (PAL) image, double-buffered, in SRAM; core 1
// adds the borders.
#define DVI_FRAME_W AMIGA_SOURCE_WIDTH
#define DVI_FRAME_H AMIGA_SOURCE_HEIGHT
static dvi_pixel_t sram_frames[2][DVI_FRAME_W * DVI_FRAME_H]
    __attribute__((aligned(4)));
#else
// Two compact scanout buffers occupy DF1's 2 MB region after the optional
// 512 KB SD ROM cache. HDMI builds therefore keep DF1 disabled; DF0 is
// unaffected. The aligned stride is large enough for the taller PAL frame.
#define DVI_FRAME_W SOURCE_WIDTH
#define DVI_FRAME_H SOURCE_HEIGHT
#define DVI_FRAME_STRIDE 0x1a000u
#define DVI_FRAME0_OFFSET (PSRAM_DF1_OFFSET + PSRAM_SD_ROM_SIZE)
#define DVI_FRAME1_OFFSET (DVI_FRAME0_OFFSET + DVI_FRAME_STRIDE)

_Static_assert(SOURCE_WIDTH * SOURCE_HEIGHT * sizeof(dvi_pixel_t) <=
                   DVI_FRAME_STRIDE,
               "DVI RGB332 frame does not fit its PSRAM stride");
_Static_assert(DVI_FRAME1_OFFSET + DVI_FRAME_STRIDE <= PSRAM_FRAMEBUF_OFFSET,
               "DVI RGB332 buffers overrun the DF1 PSRAM region");
#endif
#define DVI_FRAME_BYTES (DVI_FRAME_W * DVI_FRAME_H * sizeof(dvi_pixel_t))

static struct dvi_inst dvi;
static dvi_pixel_t *source_frames[2];
static bool source_frame_has_emulator[2];
static volatile int displayed_frame;
static volatile int pending_frame;
static uint64_t boot_pattern_until_us;
static uint32_t scanline_words[
    SOURCE_WIDTH * sizeof(dvi_pixel_t) / sizeof(uint32_t)]
    __attribute__((aligned(4)));
#if DVI_FULL_WIDTH
// Not const: .rodata lives in flash, and XIP reads on core 1 compete with the
// emulator's PSRAM traffic on the shared QMI bus (late, red border lines).
static uint32_t blank_words[count_of(scanline_words)];
// Border colour (COLOR00) of each frame. Like a real Amiga, the areas around
// the image show the border colour rather than black.
static dvi_pixel_t frame_border[2];
#if !DVI_USE_SIO_TMDS_ENCODER || DVI_SYMBOLS_PER_WORD != 2
#error "Full-width DVI scanout requires the RP2350 SIO TMDS encoder"
#endif
// PicoDVI only exposes pixel-doubled 8bpp encoding. Configure the RP2350 SIO
// TMDS encoder for one symbol per RGB332 pixel, with hardware DC balance.
static inline void __not_in_flash_func(dvi_encode_channel_fullres_8bpp)(
    const uint32_t *pixels, uint32_t *symbols, uint n_pix,
    uint channel_msb, uint channel_lsb) {
    sio_hw->tmds_ctrl =
        SIO_TMDS_CTRL_CLEAR_BALANCE_BITS |
        ((channel_msb - channel_lsb) << SIO_TMDS_CTRL_L0_NBITS_LSB) |
        (((channel_msb - 7u) & 0xfu) << SIO_TMDS_CTRL_L0_ROT_LSB) |
        ((1u + __builtin_ctz(8u)) << SIO_TMDS_CTRL_PIX_SHIFT_LSB);
    // Four 8-bit pixels in, two words of two symbols out.
    tmds_encode_sio_loop_poppop_ratio2(pixels, symbols, n_pix);
}
#endif

// Returns the first pixel of image row y (0..AMIGA_SOURCE_HEIGHT-1).
static inline dvi_pixel_t *dvi_image_row(dvi_pixel_t *frame, uint y) {
#if DVI_FULL_WIDTH
    return frame + y * DVI_FRAME_W;
#else
    return frame + (DVI_IMAGE_Y0 + y) * SOURCE_WIDTH + DVI_IMAGE_X0;
#endif
}

static inline uint8_t argb_to_dvi_pixel(uint32_t pixel) {
    return (uint8_t)(((pixel >> 16) & 0xe0u) |
                     ((pixel >> 11) & 0x1cu) |
                     ((pixel >> 6) & 0x03u));
}

static bool dvi_begin_submit(int *target_out, dvi_pixel_t **dst_out) {
    // Keep the diagnostic pattern visible long enough to distinguish DVI
    // bring-up from emulator video on a monitor or capture device.
    if (time_us_64() < boot_pattern_until_us)
        return false;

    if (__atomic_load_n(&pending_frame, __ATOMIC_ACQUIRE) >= 0)
        return false;

    int target = __atomic_load_n(&displayed_frame, __ATOMIC_ACQUIRE) ^ 1;
    dvi_pixel_t *dst = source_frames[target];
    if (!source_frame_has_emulator[target]) {
        memset(dst, 0, DVI_FRAME_BYTES);
        source_frame_has_emulator[target] = true;
    }
    *target_out = target;
    *dst_out = dst;
    return true;
}

static void dvi_publish_frame(int target) {
    __atomic_store_n(&pending_frame, target, __ATOMIC_RELEASE);
}

bool dvi_display_submit_frame(const uint32_t *argb_frame) {
    // Never overwrite a buffer that is displayed or already queued. With two
    // buffers, dropping a late producer frame is safer than tearing scanout.
    int target;
    dvi_pixel_t *dst;
    if (!dvi_begin_submit(&target, &dst))
        return false;

    // Borders stay black after the first frame, so only update the image
    // rectangle. This avoids a full-frame write on subsequent frames.
    // The presented frame's corner always holds the border colour.
    const dvi_pixel_t border_pixel = argb_to_dvi_pixel(argb_frame[0]);
    frame_border[target] = border_pixel;
    for (uint y = 0; y < AMIGA_SOURCE_HEIGHT; ++y) {
        dvi_pixel_t *dst_row = dvi_image_row(dst, y);
        // The presented framebuffer holds SCREEN_H / 2 logical rows.
        if (y >= SCREEN_H / 2u) {
            for (uint x = 0; x < AMIGA_SOURCE_WIDTH; ++x)
                dst_row[x] = border_pixel;
            continue;
        }
        const uint32_t *src_row = argb_frame + (y * 2u) * SCREEN_W;
        for (uint x = 0; x < AMIGA_SOURCE_WIDTH; ++x)
            dst_row[x] = argb_to_dvi_pixel(
                src_row[x * (DVI_FULL_WIDTH ? 1u : 2u)]);
    }

    dvi_publish_frame(target);
    return true;
}

bool dvi_display_submit_raster(const uint32_t *argb_raster,
                               uint32_t border,
                               int source_step,
                               int y_offset,
                               int row_rotation) {
    int target;
    dvi_pixel_t *dst;
    if (!dvi_begin_submit(&target, &dst))
        return false;

    const dvi_pixel_t border_pixel = argb_to_dvi_pixel(border);
    frame_border[target] = border_pixel;
    const int first_source_y = y_offset / 2;
    const int source_rows = HOST_RASTER_H / source_step;

    for (int y = 0; y < AMIGA_SOURCE_HEIGHT; ++y) {
        dvi_pixel_t *dst_row = dvi_image_row(dst, (uint)y);
        int source_y = y - first_source_y;
        if (source_y < 0 || source_y >= source_rows) {
            for (uint x = 0; x < AMIGA_SOURCE_WIDTH; ++x)
                dst_row[x] = border_pixel;
            continue;
        }

        const uint32_t *src_row =
            argb_raster + source_y * source_step * SCREEN_W;
        for (int x = 0; x < AMIGA_SOURCE_WIDTH; ++x) {
            int source_x = x * (DVI_FULL_WIDTH ? 1 : 2) + row_rotation;
            if (source_x >= SCREEN_W)
                source_x -= SCREEN_W;
            dst_row[x] = argb_to_dvi_pixel(src_row[source_x]);
        }
    }

    dvi_publish_frame(target);
    return true;
}

static void __not_in_flash_func(dvi_core1)(void) {
    // Notify core 0 that runtime_run_per_core_initializers() has completed.
    multicore_fifo_push_blocking(0u);
    // Do not touch PSRAM until core 0 has validated it and cleared the source
    // buffers following the core-1 boot-ROM initializers.
    multicore_fifo_pop_blocking();

    dvi_register_irqs_this_core(&dvi, DMA_IRQ_0);
    dvi_start(&dvi);
    // Do not let core 0 begin the emulator's heavy PSRAM traffic until PIO,
    // DMA and the TMDS queues are live on this core.
    multicore_fifo_push_blocking(0u);
    uint y = 0;
    int active_frame = 0;
#if DVI_FULL_WIDTH
    uint32_t border_words = 0;
#endif
    for (;;) {
        if (y == 0) {
            int next = __atomic_load_n(&pending_frame, __ATOMIC_ACQUIRE);
            if (next >= 0) {
                active_frame = next;
                // Publish the new consumer before making the old buffer
                // available to the producer.
                __atomic_store_n(&displayed_frame, active_frame,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&pending_frame, -1, __ATOMIC_RELEASE);
            }
#if DVI_FULL_WIDTH
            // Repaint the side columns and the border line only when the
            // border colour changes (at most once per frame).
            const uint32_t words = frame_border[active_frame] * 0x01010101u;
            if (words != border_words) {
                border_words = words;
                for (uint i = 0; i < count_of(blank_words); ++i)
                    blank_words[i] = words;
                for (uint i = 0; i < DVI_IMAGE_X0 / sizeof(uint32_t); ++i) {
                    scanline_words[i] = words;
                    scanline_words[count_of(scanline_words) - 1u - i] = words;
                }
            }
#endif
        }

        uint32_t *tmds;
        queue_remove_blocking_u32(&dvi.q_tmds_free, &tmds);
#if DVI_FULL_WIDTH
        // Only the image rows live in SRAM. Border columns of scanline_words
        // and the border line hold the frame's border colour.
        const uint32_t *pixels = blank_words;
        const uint image_y = y - DVI_IMAGE_Y0;
        if (image_y < AMIGA_SOURCE_HEIGHT) {
            const uint32_t *source = (const uint32_t *)dvi_image_row(
                source_frames[active_frame], image_y);
            uint32_t *dest = scanline_words + DVI_IMAGE_X0 / sizeof(uint32_t);
            for (uint i = 0; i < AMIGA_SOURCE_WIDTH / sizeof(uint32_t); ++i)
                dest[i] = source[i];
            pixels = scanline_words;
        }
#else
        const uint32_t *source = (const uint32_t *)(
            source_frames[active_frame] + y * SOURCE_WIDTH);
        // Copy once into SRAM before the three channel encoders traverse it.
        // Cached sequential reads are required to keep TMDS encoding ahead of
        // scanout. DF0 track reads stay in SRAM, avoiding competing PSRAM IO.
        // Keep this loop inline: libc memcpy executes from flash, and XIP
        // misses on the shared QMI bus make scanlines late (red lines).
        for (uint i = 0; i < count_of(scanline_words); ++i)
            scanline_words[i] = source[i];
        const uint32_t *pixels = scanline_words;
#endif
        const uint active_width = dvi.timing->h_active_pixels;
        const uint words_per_lane = active_width / DVI_SYMBOLS_PER_WORD;
#if DVI_FULL_WIDTH
        dvi_encode_channel_fullres_8bpp(
            pixels, tmds, active_width,
            DVI_8BPP_BLUE_MSB, DVI_8BPP_BLUE_LSB);
        dvi_encode_channel_fullres_8bpp(
            pixels, tmds + words_per_lane, active_width,
            DVI_8BPP_GREEN_MSB, DVI_8BPP_GREEN_LSB);
        dvi_encode_channel_fullres_8bpp(
            pixels, tmds + 2u * words_per_lane, active_width,
            DVI_8BPP_RED_MSB, DVI_8BPP_RED_LSB);
#else
        tmds_encode_data_channel_8bpp(
            pixels, tmds, active_width / 2u,
            DVI_8BPP_BLUE_MSB, DVI_8BPP_BLUE_LSB);
        tmds_encode_data_channel_8bpp(
            pixels, tmds + words_per_lane, active_width / 2u,
            DVI_8BPP_GREEN_MSB, DVI_8BPP_GREEN_LSB);
        tmds_encode_data_channel_8bpp(
            pixels, tmds + 2u * words_per_lane, active_width / 2u,
            DVI_8BPP_RED_MSB, DVI_8BPP_RED_LSB);
#endif
        queue_add_blocking_u32(&dvi.q_tmds_valid, &tmds);
        y = (y + 1u) % SOURCE_HEIGHT;
    }
}

void dvi_display_init(void) {
#if DVI_FULL_WIDTH
    source_frames[0] = sram_frames[0];
    source_frames[1] = sram_frames[1];
#else
    source_frames[0] = (dvi_pixel_t *)psram_ptr(DVI_FRAME0_OFFSET);
    source_frames[1] = (dvi_pixel_t *)psram_ptr(DVI_FRAME1_OFFSET);
#endif
    source_frame_has_emulator[0] = false;
    source_frame_has_emulator[1] = false;
    displayed_frame = 0;
    pending_frame = -1;

    // RP2350B PIO instances address either GPIO0..31 or GPIO16..47. Select
    // the window containing this board's four differential output pairs.
    if (pio_set_gpio_base(pio0, BOARD_DVI_PIO_GPIO_BASE) != PICO_OK) {
        panic("Unable to select RP2350B PIO GPIO base");
    }

#if OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL
    dvi.timing = &amiga_dvi_timing;
#else
    dvi.timing = &dvi_timing_640x480p_60hz;
#endif
    // PicoDVI serialises one TMDS bit per clk_sys cycle; any mismatch yields a
    // non-standard mode that monitors and capture devices may reject.
    if (dvi.timing->h_active_pixels != DVI_ACTIVE_WIDTH ||
        clock_get_hz(clk_sys) != dvi.timing->bit_clk_khz * 1000u)
        panic("DVI timing does not match clk_sys or scanout width");
    dvi.ser_cfg = (struct dvi_serialiser_cfg) {
        .pio = pio0,
        .sm_tmds = {0, 1, 2},
        .pins_tmds = {
            BOARD_DVI_TMDS_D0_PIN,
            BOARD_DVI_TMDS_D1_PIN,
            BOARD_DVI_TMDS_D2_PIN,
        },
        .pins_clk = BOARD_DVI_TMDS_CLK_PIN,
        .invert_diffpairs = BOARD_DVI_INVERT_DIFF,
    };
    dvi_init(&dvi, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    multicore_launch_core1(dvi_core1);
}

static void dvi_fill_boot_pattern(dvi_pixel_t *frame) {
    static const uint8_t bars[8] = {
        0xe0, // red
        0xfc, // yellow
        0x1c, // green
        0x1f, // cyan
        0x03, // blue
        0xe3, // magenta
        0xff, // white
        0x00, // black
    };

    for (uint y = 0; y < DVI_FRAME_H; ++y) {
        for (uint x = 0; x < DVI_FRAME_W; ++x) {
            dvi_pixel_t pixel = bars[(x * count_of(bars)) / DVI_FRAME_W];
            // White frame makes cropping and vertical stability obvious.
            if (x < 4u || x >= DVI_FRAME_W - 4u ||
                y < 4u || y >= DVI_FRAME_H - 4u)
                pixel = 0xffu;
            frame[y * DVI_FRAME_W + x] = pixel;
        }
    }
}

void dvi_display_start(void) {
    dvi_fill_boot_pattern(source_frames[0]);
    dvi_fill_boot_pattern(source_frames[1]);
    boot_pattern_until_us = time_us_64() + 1500000u;

    // Frame presentation performs large PSRAM writes on core 0. Give scanout
    // and its DMA channels priority so those writes cannot starve PicoDVI.
    hw_set_bits(&busctrl_hw->priority,
                BUSCTRL_BUS_PRIORITY_PROC1_BITS |
                BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
                BUSCTRL_BUS_PRIORITY_DMA_W_BITS);
    hw_clear_bits(&busctrl_hw->priority, BUSCTRL_BUS_PRIORITY_PROC0_BITS);
    while (!busctrl_hw->priority_ack)
        tight_loop_contents();

    multicore_fifo_push_blocking(0u);
    multicore_fifo_pop_blocking();
    printf("DVI: PIO %s emulator scanout on %s (%s)\n",
           DVI_MODE_NAME, BOARD_DVI_PIN_RANGE,
           DVI_FULL_WIDTH ? "SRAM frames" : "shared cached PSRAM");
}
