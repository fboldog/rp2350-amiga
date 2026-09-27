// PIO DVI bring-up for supported RP2350B boards.
//
// PicoDVI performs TMDS encoding on core 1 and serialises three data lanes
// with PIO. Core 0 converts completed emulator frames to double-buffered
// RGB332 in PSRAM; core 1 changes buffers only at a DVI frame boundary.

#include "dvi_display.h"
#include "board_config.h"
#include "Host.h"
#include "psram.h"
#include "../omega/VideoStandard.h"

#include "dvi.h"
#include "tmds_encode.h"
#include "hardware/dma.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/busctrl.h"
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
#define SOURCE_WIDTH       360u
#define SOURCE_HEIGHT      288u
#define AMIGA_SOURCE_WIDTH 320u
#define AMIGA_SOURCE_HEIGHT 200u
#define DVI_MODE_NAME      "720x576p50 (640x400 image doubled)"
#else
#define SOURCE_WIDTH       360u
#define SOURCE_HEIGHT      240u
#define AMIGA_SOURCE_WIDTH 320u
#define AMIGA_SOURCE_HEIGHT 200u
#define DVI_MODE_NAME      "720x480p60 (640x400 image doubled)"
#endif

// Two compact scanout buffers occupy DF1's 2 MB region after the optional
// 512 KB SD ROM cache. HDMI builds therefore keep DF1 disabled; DF0 is
// unaffected. The aligned stride is large enough for the taller PAL frame.
#define DVI_FRAME_STRIDE 0x1a000u
#define DVI_FRAME0_OFFSET (PSRAM_DF1_OFFSET + PSRAM_SD_ROM_SIZE)
#define DVI_FRAME1_OFFSET (DVI_FRAME0_OFFSET + DVI_FRAME_STRIDE)

_Static_assert(SOURCE_WIDTH * SOURCE_HEIGHT <= DVI_FRAME_STRIDE,
               "DVI RGB332 frame does not fit its PSRAM stride");
_Static_assert(DVI_FRAME1_OFFSET + DVI_FRAME_STRIDE <= PSRAM_FRAMEBUF_OFFSET,
               "DVI RGB332 buffers overrun the DF1 PSRAM region");

static struct dvi_inst dvi;
static uint8_t *source_frames[2];
static volatile int displayed_frame;
static volatile int pending_frame;

static uint8_t *dvi_psram_uncached_ptr(uint32_t offset) {
    // The producer and consumer run on different cores. Use RP2350's
    // no-cache/no-allocate XIP alias so a completed frame becomes visible to
    // core 1 immediately and scanout does not evict emulator working data.
    uintptr_t cached = (uintptr_t)psram_ptr(offset);
    return (uint8_t *)(cached + XIP_NOCACHE_NOALLOC_BASE - XIP_BASE);
}

static inline uint8_t argb_to_rgb332(uint32_t pixel) {
    return (uint8_t)(((pixel >> 16) & 0xe0u) |
                     ((pixel >> 11) & 0x1cu) |
                     ((pixel >> 6) & 0x03u));
}

bool dvi_display_submit_frame(const uint32_t *argb_frame) {
    // Never overwrite a buffer that is displayed or already queued. With two
    // buffers, dropping a late producer frame is safer than tearing scanout.
    if (__atomic_load_n(&pending_frame, __ATOMIC_ACQUIRE) >= 0)
        return false;

    int target = __atomic_load_n(&displayed_frame, __ATOMIC_ACQUIRE) ^ 1;
    uint8_t *dst = source_frames[target];
    const uint x0 = (SOURCE_WIDTH - AMIGA_SOURCE_WIDTH) / 2u;
    const uint y0 = (SOURCE_HEIGHT - AMIGA_SOURCE_HEIGHT) / 2u;

    // Both frames were cleared at startup and their borders stay black, so
    // only update the image rectangle. This avoids a full-frame PSRAM write.
    for (uint y = 0; y < AMIGA_SOURCE_HEIGHT; ++y) {
        const uint32_t *src_row = argb_frame + (y * 2u) * SCREEN_W;
        uint8_t *dst_row = dst + (y0 + y) * SOURCE_WIDTH + x0;
        for (uint x = 0; x < AMIGA_SOURCE_WIDTH; ++x)
            dst_row[x] = argb_to_rgb332(src_row[x * 2u]);
    }

    __atomic_store_n(&pending_frame, target, __ATOMIC_RELEASE);
    return true;
}

static void __not_in_flash_func(dvi_core1)(void) {
    // Core 1 has a dedicated 2 KB scratch-bank stack. Keeping this staging
    // line there frees enough main SRAM for PicoDVI's required third TMDS
    // buffer without adding any PSRAM traffic to the encoder.
    uint32_t scanline_words[SOURCE_WIDTH / sizeof(uint32_t)]
        __attribute__((aligned(4)));

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
        }

        uint32_t *tmds;
        queue_remove_blocking_u32(&dvi.q_tmds_free, &tmds);
        const uint32_t *source = (const uint32_t *)(
            source_frames[active_frame] + y * SOURCE_WIDTH);
        // Each RGB channel encoder traverses its input separately. Copying the
        // line once means 360 uncached PSRAM bytes rather than three passes,
        // and keeps the time-critical TMDS work entirely in SRAM.
        for (uint i = 0; i < count_of(scanline_words); ++i)
            scanline_words[i] = source[i];
        const uint32_t *pixels = scanline_words;
        const uint active_width = dvi.timing->h_active_pixels;
        const uint words_per_lane = active_width / DVI_SYMBOLS_PER_WORD;
        tmds_encode_data_channel_8bpp(
            pixels, tmds, active_width / 2u,
            DVI_8BPP_BLUE_MSB, DVI_8BPP_BLUE_LSB);
        tmds_encode_data_channel_8bpp(
            pixels, tmds + words_per_lane, active_width / 2u,
            DVI_8BPP_GREEN_MSB, DVI_8BPP_GREEN_LSB);
        tmds_encode_data_channel_8bpp(
            pixels, tmds + 2u * words_per_lane, active_width / 2u,
            DVI_8BPP_RED_MSB, DVI_8BPP_RED_LSB);
        queue_add_blocking_u32(&dvi.q_tmds_valid, &tmds);
        y = (y + 1u) % SOURCE_HEIGHT;
    }
}

void dvi_display_init(void) {
    source_frames[0] = dvi_psram_uncached_ptr(DVI_FRAME0_OFFSET);
    source_frames[1] = dvi_psram_uncached_ptr(DVI_FRAME1_OFFSET);
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
    dvi.timing = &dvi_timing_720x480p_60hz;
#endif
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

void dvi_display_start(void) {
    memset(source_frames[0], 0, SOURCE_WIDTH * SOURCE_HEIGHT);
    memset(source_frames[1], 0, SOURCE_WIDTH * SOURCE_HEIGHT);

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
    printf("DVI: PIO %s emulator scanout on %s (uncached PSRAM)\n",
           DVI_MODE_NAME, BOARD_DVI_PIN_RANGE);
}
