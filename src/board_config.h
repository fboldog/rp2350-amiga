// ===========================================================================
//  board_config.h  -  Waveshare RP2350-PiZero  (Omega Amiga emulator port)
// ===========================================================================
//
//  Central hardware definition for the target board.  Every RP2350-specific
//  source file (src/psram.*, src/main.c, src/Host.*) includes this.
//
//  Board       : Waveshare RP2350-PiZero
//  MCU         : RP2350B  (dual Cortex-M33 / Hazard3, 80-pin, GPIO0..47)
//  Flash       : 16 MB QSPI  (W25Q128, boot ROM at 0x10000000)
//  PSRAM       : 8-pin QSPI footprint on the shared QMI bus, CS = GPIO47
//                *** NOT populated from the factory - must be soldered ***
//  Video       : DVI/HDMI via HSTX-adjacent GPIOs 32..39 (PicoDVI-style PIO)
//  Storage     : microSD (SPI1 or 4-bit SDIO)
//  USB         : native USB-C (host or device) + optional PIO-USB port
//
//  Pin data cross-checked against:
//    - Waveshare "RP2350-PiZero.pdf" schematic (files.waveshare.com/wiki/RP2350-PiZero/)
//    - Waveshare RP2350-PiZero demo pack (01-DVI common_dvi_pin_configs.h =
//      "pico_sock_cfg"; 03-MicroSD hw_config.c)
//    - RP2350 datasheet GPIO function table
//
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
//  Board identity / guard
// ---------------------------------------------------------------------------
#define BOARD_WAVESHARE_RP2350_PIZERO 1
#define BOARD_NAME "Waveshare RP2350-PiZero"

#if !defined(PICO_RP2350B) && !defined(WAVESHARE_RP2350_PIZERO)
// You are building without the matching Pico SDK board header. That is OK
// (the code below does not depend on it) but PICO_BOARD should be either
// "waveshare_rp2350_pizero" (see src/boards/) or a generic RP2350B target.
#endif

// ---------------------------------------------------------------------------
//  System clock
// ---------------------------------------------------------------------------
//  250 MHz is the emulator's working point (VREG 1.15 V, set in main.c).
//  For DVI at 640x480p60 the pixel clock chain wants a sys clock that is an
//  integer multiple of 251.75 MHz/N; PicoDVI's dvi_timing uses 252 MHz for
//  full-res or ~126 MHz for the "scanbuf" path. If you enable DVI, revisit
//  BOARD_SYS_CLK_KHZ so CPU speed and the DVI bit clock agree.
#ifndef BOARD_SYS_CLK_KHZ
#define BOARD_SYS_CLK_KHZ   250000u
#endif
#define BOARD_VREG_VOLTAGE  VREG_VOLTAGE_1_15

// ---------------------------------------------------------------------------
//  Debug UART  (UART0, 115200 8N1)
// ---------------------------------------------------------------------------
#define BOARD_UART_ID        uart0
#define BOARD_UART_BAUD      115200
#define BOARD_UART_TX_PIN    0
#define BOARD_UART_RX_PIN    1

// ---------------------------------------------------------------------------
//  Status LED  (single WS2812 / NeoPixel, not a plain GPIO LED)
// ---------------------------------------------------------------------------
#define BOARD_WS2812_PIN     2
#define BOARD_HAS_PLAIN_LED  0

// ---------------------------------------------------------------------------
//  PSRAM  (QMI CS1)
// ---------------------------------------------------------------------------
//  The PiZero ships with an EMPTY PSRAM pad (U1). This project needs PSRAM for
//  all Amiga RAM, so you must fit an 8-pin QSPI PSRAM (APS6404L / LY68L6400 /
//  ESP-PSRAM64H class - 3.3 V, quad, 0x35 "enter QPI", 0xEB fast-read).
//
//  Set BOARD_HAS_PSRAM to 1 once fitted. A single 8-pin die is 8 MB (64 Mbit)
//  max; "16 MB" would require a stacked/dual part and a second CS, which this
//  board does not route - treat 8 MB as the ceiling here.
#ifndef BOARD_HAS_PSRAM
#define BOARD_HAS_PSRAM      1
#endif

#define BOARD_PSRAM_CS_PIN       47          // QMI CS1  (GPIO_FUNC_XIP_CS1)
#define BOARD_PSRAM_BASE         0x11000000u // CS1 XIP window
#ifndef BOARD_PSRAM_SIZE_BYTES
#define BOARD_PSRAM_SIZE_BYTES   0x800000u   // 8 MB  (set to your fitted part)
#endif

//  QMI CS1 timing. clkdiv=2 -> QSPI at sys/2 (125 MHz @ 250 MHz sys), within
//  APS6404L's 133 MHz quad-read rating. Raise to 3 if reads are unstable.
#ifndef BOARD_PSRAM_CLKDIV
#define BOARD_PSRAM_CLKDIV       2u
#endif
#define BOARD_PSRAM_RXDELAY      1u
#define BOARD_PSRAM_READ_DUMMY   6u          // 0xEB quad fast-read dummy cycles

// ---------------------------------------------------------------------------
//  DVI / HDMI output   (differential pairs are {P = pin, N = pin+1})
// ---------------------------------------------------------------------------
//  Matches PicoDVI's "pico_sock_cfg" that Waveshare's 01-DVI demo selects.
#define BOARD_HAS_DVI            1
#define BOARD_DVI_TMDS_D0_PIN    36    // pair 36/37  (blue)
#define BOARD_DVI_TMDS_D1_PIN    34    // pair 34/35  (green)
#define BOARD_DVI_TMDS_D2_PIN    32    // pair 32/33  (red)
#define BOARD_DVI_TMDS_CLK_PIN   38    // pair 38/39
#define BOARD_DVI_INVERT_DIFF    0     // pico_sock_cfg: invert_diffpairs = false
#define BOARD_DVI_PIO            pio0
#define BOARD_DVI_GPIO_BASE      16    // pio_set_gpio_base(pio, 16) - pins are >=16

//  HDMI DDC (EDID) + CEC - not needed for blind video output, wire later if
//  you want mode negotiation.
#define BOARD_DVI_DDC_SDA_PIN    44
#define BOARD_DVI_DDC_SCL_PIN    45
#define BOARD_DVI_CEC_PIN        46

//  PicoDVI dvi_serialiser_cfg initialiser (drop into libdvi):
//    struct dvi_serialiser_cfg cfg = BOARD_DVI_SERIALISER_CFG;
#define BOARD_DVI_SERIALISER_CFG {                    \
    .pio             = BOARD_DVI_PIO,                 \
    .sm_tmds         = {0, 1, 2},                     \
    .pins_tmds       = {BOARD_DVI_TMDS_D0_PIN,        \
                        BOARD_DVI_TMDS_D1_PIN,        \
                        BOARD_DVI_TMDS_D2_PIN},       \
    .pins_clk        = BOARD_DVI_TMDS_CLK_PIN,        \
    .invert_diffpairs = BOARD_DVI_INVERT_DIFF        \
}

// ---------------------------------------------------------------------------
//  microSD card
// ---------------------------------------------------------------------------
//  The slot is wired for BOTH 1-bit SPI and 4-bit SDIO (mux resistors).
//  SPI path (simplest, matches Waveshare's no-OS-FatFS hw_config.c):
#define BOARD_HAS_SDCARD        1
#define BOARD_SD_SPI            spi1
#define BOARD_SD_SCK_PIN        30    // SPI1 SCK
#define BOARD_SD_MOSI_PIN       31    // SPI1 TX   (SD CMD)
#define BOARD_SD_MISO_PIN       40    // SPI1 RX   (SD DAT0)
#define BOARD_SD_CS_PIN         43    // plain GPIO (SD DAT3)
#define BOARD_SD_SPI_BAUD       12500000u   // 12.5 MHz init-safe; raise after init

//  SDIO path (4-bit, faster) - D0..D3 are contiguous from BOARD_SD_SDIO_D0_PIN:
#define BOARD_SD_SDIO_CLK_PIN   38    // NOTE: shared with DVI clock - SDIO and
                                     //       DVI cannot both be used.
#define BOARD_SD_SDIO_CMD_PIN   31
#define BOARD_SD_SDIO_D0_PIN    40    // D1=41, D2=42, D3=43

// ---------------------------------------------------------------------------
//  USB
// ---------------------------------------------------------------------------
//  Native USB-C: RP2350 dedicated USB_DP/DM. Use for TinyUSB host (HID
//  keyboard/mouse) or device. No GPIOs needed.
#define BOARD_HAS_NATIVE_USB   1

//  Optional second "PIO-USB" port (bit-banged). Waveshare's demo leaves the
//  D+ pin to the sketch; confirm against your board's silkscreen/schematic
//  before relying on this value.
#define BOARD_HAS_PIO_USB      1
#ifndef BOARD_PIO_USB_DP_PIN
#define BOARD_PIO_USB_DP_PIN   12    // D- = DP+1 = 13   (VERIFY against schematic)
#endif

// ---------------------------------------------------------------------------
//  Amiga memory map inside PSRAM   (emulator layout, not board wiring)
// ---------------------------------------------------------------------------
//  0x000000..0x1FFFFF  Chip RAM        (2 MB)      Amiga 0x000000..0x1FFFFF
//  0x200000..0x27FFFF  Slow/Ranger RAM (512 KB)    Amiga 0xC00000..0xC7FFFF
//  0x280000..0x47FFFF  DF0 MFM buffer  (2 MB)
//  0x480000..0x67FFFF  DF1 MFM buffer  (2 MB)
//  0x680000..0x77FFFF  Framebuffer 640x400 ARGB32 (1 MB)
//  0x780000..0x7FCFFF  Video DMA raster scratch (640x200 ARGB32, 500 KB)
//  0x7FD000..0x7FFFFF  Reserved (12 KB)
#define BOARD_MAP_CHIPRAM_OFFSET   0x000000u
#define BOARD_MAP_SLOWRAM_OFFSET   0x200000u
#define BOARD_MAP_DF0_OFFSET       0x280000u
#define BOARD_MAP_DF1_OFFSET       0x480000u
#define BOARD_MAP_FRAMEBUF_OFFSET  0x680000u
#define BOARD_MAP_VIDEO_RASTER_OFFSET 0x780000u

#define BOARD_MAP_CHIPRAM_SIZE     0x200000u   // 2 MB
#define BOARD_MAP_SLOWRAM_SIZE     0x080000u   // 512 KB
#define BOARD_MAP_FLOPPY_SIZE      0x200000u   // 2 MB per drive

//  Kickstart ROM in flash (place with tools/combine_uf2.py at offset 0x200000)
#define BOARD_ROM_FLASH_BASE       0x10200000u
#define BOARD_ROM_SIZE             0x080000u   // 512 KB (256 KB ROMs mirrored)

// ---------------------------------------------------------------------------
//  Compile-time sanity checks
// ---------------------------------------------------------------------------
#if BOARD_HAS_PSRAM
_Static_assert(BOARD_MAP_FRAMEBUF_OFFSET + 640u*400u*4u <= BOARD_PSRAM_SIZE_BYTES,
               "framebuffer overruns fitted PSRAM - shrink the map or fit more PSRAM");
_Static_assert(BOARD_MAP_VIDEO_RASTER_OFFSET + 640u*178u*4u <= BOARD_PSRAM_SIZE_BYTES,
               "video raster scratch buffer overruns fitted PSRAM");
#endif
#if BOARD_HAS_DVI && BOARD_HAS_SDCARD
//  DVI uses GPIO 32..39; SDIO clock is GPIO38. SPI-SD (30/31/40/43) is clear of
//  the DVI pins, so DVI + SPI-SD is fine; DVI + SDIO is not.
_Static_assert(BOARD_SD_SCK_PIN < 32 || BOARD_SD_SCK_PIN > 39,
               "SPI-SD clock collides with the DVI TMDS pin block (32..39)");
#endif
