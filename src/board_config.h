// ===========================================================================
//  board_config.h  -  RP2350B boards used by the Omega Amiga emulator port
// ===========================================================================
//
//  Central hardware definition for the target board.  Every RP2350-specific
//  source file (src/psram.*, src/main.c, src/Host.*) includes this.
//
//  Boards      : Waveshare RP2350-PiZero, WeAct Studio RP2350B Core
//  MCU         : RP2350B (dual Cortex-M33 / Hazard3, 80-pin, GPIO0..47)
//  Flash       : 16 MB QSPI
//  PSRAM       : 8 MB QSPI, CS = GPIO47 (Waveshare) or GPIO0 (WeAct)
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
#if defined(OMEGA_BOARD_WEACT)
#define BOARD_WEACT_STUDIO_RP2350B_CORE 1
#define BOARD_NAME "WeAct Studio RP2350B Core"
#define BOARD_UART_ID        uart1
#define BOARD_UART_BAUD      115200
#define BOARD_UART_TX_PIN    4
#define BOARD_UART_RX_PIN    5
#define BOARD_HAS_PLAIN_LED  1
#define BOARD_LED_PIN        25
#define BOARD_HAS_DVI        1
#define BOARD_HAS_SDCARD     0
#define BOARD_HAS_PIO_USB    0
#elif defined(OMEGA_BOARD_WAVESHARE)
#define BOARD_WAVESHARE_RP2350_PIZERO 1
#define BOARD_NAME "Waveshare RP2350-PiZero"
#define BOARD_UART_ID        uart0
#define BOARD_UART_BAUD      115200
#define BOARD_UART_TX_PIN    0
#define BOARD_UART_RX_PIN    1
#define BOARD_WS2812_PIN     2
#define BOARD_HAS_PLAIN_LED  0
#define BOARD_HAS_DVI        1
#define BOARD_HAS_SDCARD     1
#define BOARD_HAS_PIO_USB    1
#else
#error "Select a supported board through CMake PICO_BOARD"
#endif

// ---------------------------------------------------------------------------
//  System clock
// ---------------------------------------------------------------------------
// Normal builds retain the SDK's 150 MHz startup clock. HDMI builds switch to
// PicoDVI's TMDS bit clock and reconfigure PSRAM through the public SDK API.
#ifndef OMEGA_BOARD_SYS_CLK_KHZ
#define OMEGA_BOARD_SYS_CLK_KHZ   150000u
#endif

// ---------------------------------------------------------------------------
//  PSRAM  (QMI CS1)
// ---------------------------------------------------------------------------
// The WeAct board has populated PSRAM. The Waveshare board needs a compatible
// chip fitted to its empty U1 footprint. hardware_psram configures QMI at SDK
// startup using the CMake-provided PICO_PSRAM_CS_PIN.
#define BOARD_HAS_PSRAM          1
#define BOARD_PSRAM_CS_PIN       PICO_PSRAM_CS_PIN
#define BOARD_PSRAM_BASE         0x11000000u // CS1 XIP window
#ifndef BOARD_PSRAM_SIZE_BYTES
#define BOARD_PSRAM_SIZE_BYTES   PICO_PSRAM_SIZE_BYTES
#endif

// ---------------------------------------------------------------------------
//  DVI / HDMI output   (differential pairs are {P = pin, N = pin+1})
// ---------------------------------------------------------------------------
// Both boards use PicoDVI's PIO serialiser. On WeAct, GPIO11..18 line up with
// an external Adafruit DVI breakout; Waveshare uses its on-board connector.
#if BOARD_HAS_DVI
#if BOARD_WEACT_STUDIO_RP2350B_CORE
#define BOARD_DVI_TMDS_D0_PIN    11    // pair 11/12  (blue)
#define BOARD_DVI_TMDS_D1_PIN    17    // pair 17/18  (green)
#define BOARD_DVI_TMDS_D2_PIN    15    // pair 15/16  (red)
#define BOARD_DVI_TMDS_CLK_PIN   13    // pair 13/14
#define BOARD_DVI_INVERT_DIFF    0
#define BOARD_DVI_PIO_GPIO_BASE  0
#define BOARD_DVI_PIN_RANGE      "GPIO11..18"
#elif BOARD_WAVESHARE_RP2350_PIZERO
#define BOARD_DVI_TMDS_D0_PIN    36    // pair 36/37  (blue)
#define BOARD_DVI_TMDS_D1_PIN    34    // pair 34/35  (green)
#define BOARD_DVI_TMDS_D2_PIN    32    // pair 32/33  (red)
#define BOARD_DVI_TMDS_CLK_PIN   38    // pair 38/39
#define BOARD_DVI_INVERT_DIFF    0     // pico_sock_cfg: invert_diffpairs = false
#define BOARD_DVI_PIO_GPIO_BASE  16
#define BOARD_DVI_PIN_RANGE      "GPIO32..39"

//  HDMI DDC (EDID) + CEC - not needed for blind video output, wire later if
//  you want mode negotiation.
#define BOARD_DVI_DDC_SDA_PIN    44
#define BOARD_DVI_DDC_SCL_PIN    45
#define BOARD_DVI_CEC_PIN        46
#endif
#endif

// ---------------------------------------------------------------------------
//  microSD card
// ---------------------------------------------------------------------------
//  The slot is wired for BOTH 1-bit SPI and 4-bit SDIO (mux resistors).
//  SPI path (simplest, matches Waveshare's no-OS-FatFS hw_config.c):
#if BOARD_HAS_SDCARD
#define BOARD_SD_SPI            spi1
#define BOARD_SD_SCK_PIN        30    // SPI1 SCK
#define BOARD_SD_MOSI_PIN       31    // SPI1 TX   (SD CMD)
#define BOARD_SD_MISO_PIN       40    // SPI1 RX   (SD DAT0)
#define BOARD_SD_CS_PIN         43    // plain GPIO (SD DAT3)
#define BOARD_SD_SPI_BAUD       12500000u   // 12.5 MHz init-safe; raise after init
#ifndef BOARD_SD_ROM_PATH
#define BOARD_SD_ROM_PATH       "0:/rom/kick13.rom"
#endif

//  SDIO path (4-bit, faster) - D0..D3 are contiguous from BOARD_SD_SDIO_D0_PIN:
#define BOARD_SD_SDIO_CLK_PIN   38    // NOTE: shared with DVI clock - SDIO and
                                     //       DVI cannot both be used.
#define BOARD_SD_SDIO_CMD_PIN   31
#define BOARD_SD_SDIO_D0_PIN    40    // D1=41, D2=42, D3=43
#endif

// ---------------------------------------------------------------------------
//  USB
// ---------------------------------------------------------------------------
//  Native USB-C: RP2350 dedicated USB_DP/DM. Use for TinyUSB host (HID
//  keyboard/mouse) or device. No GPIOs needed.
#define BOARD_HAS_NATIVE_USB   1

//  Optional second "PIO-USB" port (bit-banged). Waveshare's demo leaves the
//  D+ pin to the sketch; confirm against your board's silkscreen/schematic
//  before relying on this value.
#if BOARD_HAS_PIO_USB
#ifndef BOARD_PIO_USB_DP_PIN
#define BOARD_PIO_USB_DP_PIN   12    // D- = DP+1 = 13   (VERIFY against schematic)
#endif
#endif

// ---------------------------------------------------------------------------
//  Amiga memory map inside PSRAM   (emulator layout, not board wiring)
// ---------------------------------------------------------------------------
//  0x000000..0x1FFFFF  Chip RAM        (2 MB)      Amiga 0x000000..0x1FFFFF
//  0x200000..0x27FFFF  Slow/Ranger RAM (512 KB)    Amiga 0xC00000..0xC7FFFF
//  0x280000..0x47FFFF  DF0 MFM buffer  (2 MB)
//  0x480000..0x4FFFFF  SD ROM cache    (512 KB; overlaps DF1)
//  0x500000..0x533FFF  HDMI RGB332 scanout buffers (2 × 104 KB)
//  0x540000..0x639FFF  Video DMA raster scratch (640x400 ARGB32, 1000 KB)
//  0x480000..0x67FFFF  DF1 MFM buffer  (unavailable with SD ROM/HDMI)
//  0x680000..0x77FFFF  Framebuffer 640x400 ARGB32 (1 MB)
//  0x780000..0x7FFFFF  Reserved (512 KB)
#define BOARD_MAP_CHIPRAM_OFFSET   0x000000u
#define BOARD_MAP_SLOWRAM_OFFSET   0x200000u
#define BOARD_MAP_DF0_OFFSET       0x280000u
#define BOARD_MAP_DF1_OFFSET       0x480000u
#define BOARD_MAP_FRAMEBUF_OFFSET  0x680000u
#define BOARD_MAP_VIDEO_RASTER_OFFSET 0x540000u
#define BOARD_MAP_SD_ROM_OFFSET    0x480000u

#define BOARD_MAP_CHIPRAM_SIZE     0x200000u   // 2 MB
#define BOARD_MAP_SLOWRAM_SIZE     0x080000u   // 512 KB
#define BOARD_MAP_FLOPPY_SIZE      0x200000u   // 2 MB per drive
#define BOARD_MAP_SD_ROM_SIZE      0x080000u   // 512 KB maximum

//  Kickstart ROM in flash (place with tools/combine_uf2.py at offset 0x200000)
#define BOARD_ROM_FLASH_BASE       0x10200000u
#define BOARD_ROM_SIZE             0x080000u   // 512 KB (256 KB ROMs mirrored)

// ---------------------------------------------------------------------------
//  Compile-time sanity checks
// ---------------------------------------------------------------------------
#if BOARD_HAS_PSRAM
_Static_assert(BOARD_MAP_FRAMEBUF_OFFSET + 640u*400u*4u <= BOARD_PSRAM_SIZE_BYTES,
               "framebuffer overruns fitted PSRAM - shrink the map or fit more PSRAM");
_Static_assert(BOARD_MAP_VIDEO_RASTER_OFFSET + 640u*400u*4u <= BOARD_MAP_FRAMEBUF_OFFSET,
               "video raster scratch buffer overruns fitted PSRAM");
#endif
#if BOARD_HAS_DVI && BOARD_HAS_SDCARD
//  DVI uses GPIO 32..39; SDIO clock is GPIO38. SPI-SD (30/31/40/43) is clear of
//  the DVI pins, so DVI + SPI-SD is fine; DVI + SDIO is not.
_Static_assert(BOARD_SD_SCK_PIN < 32 || BOARD_SD_SCK_PIN > 39,
               "SPI-SD clock collides with the DVI TMDS pin block (32..39)");
#endif
