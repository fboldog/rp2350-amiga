// PIO DVI bring-up for the Waveshare RP2350-PiZero.
//
// PicoDVI performs TMDS encoding on core 1 and serialises three data lanes
// with PIO. A small RGB332 grid in SRAM deliberately avoids external PSRAM.

#include "dvi_display.h"
#include "board_config.h"
#include "../omega/VideoStandard.h"

#include "dvi.h"
#include "tmds_encode.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdio.h>

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
#define AMIGA_SOURCE_HEIGHT 256u
#define DVI_MODE_NAME      "720x576p50 (320x256 doubled)"
#else
#define SOURCE_WIDTH       360u
#define SOURCE_HEIGHT      240u
#define AMIGA_SOURCE_WIDTH 320u
#define AMIGA_SOURCE_HEIGHT 200u
#define DVI_MODE_NAME      "720x480p60 (320x200 doubled)"
#endif

static struct dvi_inst dvi;
static uint8_t test_lines[8][SOURCE_WIDTH] __attribute__((aligned(4)));
static uint8_t blank_line[SOURCE_WIDTH] __attribute__((aligned(4)));

static void build_test_pattern(void) {
    static const uint8_t colours[8] = {
        0xff, 0xe0, 0x1c, 0x03, 0xfc, 0xe3, 0x1f, 0x00,
    };
    const uint x0 = (SOURCE_WIDTH - AMIGA_SOURCE_WIDTH) / 2u;
    for (uint yband = 0; yband < 8; ++yband) {
        for (uint x = 0; x < SOURCE_WIDTH; ++x) {
            uint8_t colour = 0;
            if (x >= x0 && x < x0 + AMIGA_SOURCE_WIDTH) {
                uint content_x = x - x0;
                uint xband = content_x / (AMIGA_SOURCE_WIDTH / 8u);
                colour = colours[(xband + yband) & 7u];
                if (content_x < 2 || content_x >= AMIGA_SOURCE_WIDTH - 2)
                    colour = 0xff;
            }
            test_lines[yband][x] = colour;
        }
    }
}

static void __not_in_flash_func(dvi_core1)(void) {
    // Notify core 0 that runtime_run_per_core_initializers() (which calls
    // bootrom_state_reset, resetting QMI M1) has completed.  Core 0 waits
    // on this signal before calling psram_reinstate_m1() and memory_init().
    multicore_fifo_push_blocking(0u);

    dvi_register_irqs_this_core(&dvi, DMA_IRQ_0);
    dvi_start(&dvi);
    uint y = 0;
    for (;;) {
        uint32_t *tmds;
        queue_remove_blocking_u32(&dvi.q_tmds_free, &tmds);
        const uint content_y0 =
            (SOURCE_HEIGHT - AMIGA_SOURCE_HEIGHT) / 2u;
        uint yband = 0;
        if (y >= content_y0 && y < content_y0 + AMIGA_SOURCE_HEIGHT)
            yband = 1u + (y - content_y0) / (AMIGA_SOURCE_HEIGHT / 8u);
        const uint32_t *pixels = yband
            ? (const uint32_t *)test_lines[yband - 1u]
            : (const uint32_t *)blank_line;
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
    build_test_pattern();

    // RP2350B PIO instances address either GPIO0..31 or GPIO16..47. Select
    // the upper window before PicoDVI configures the GPIO32..39 side-set pins.
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
    printf("DVI: PIO %s test pattern on GPIO32..39\n", DVI_MODE_NAME);
}
