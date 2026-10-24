// ===========================================================================
//  board_config.h  -  WeAct Studio RP2350B Core for the Omega Amiga emulator
// ===========================================================================
//
//  Central hardware definition for the target board.  Every RP2350-specific
//  source file (src/psram.*, src/main.c, src/Host.*) includes this.
//
//  Board       : WeAct Studio RP2350B Core
//  MCU         : RP2350B (dual Cortex-M33 / Hazard3, 80-pin, GPIO0..47)
//  Flash       : 16 MB QSPI
//  PSRAM       : 8 MB QSPI, CS = GPIO0 (populated)
//
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>

#define BOARD_NAME "WeAct Studio RP2350B Core"

// ---------------------------------------------------------------------------
//  Debug UART, LED, user button
// ---------------------------------------------------------------------------
#define BOARD_UART_ID        uart1
#define BOARD_UART_BAUD      115200
#define BOARD_UART_TX_PIN    4
#define BOARD_UART_RX_PIN    5
#define BOARD_LED_PIN        25
#define BOARD_HAS_USER_BUTTON 1
#define BOARD_USER_BUTTON_PIN 23      // KEY: inserts/ejects the flash DF0 disk
#define BOARD_USER_BUTTON_ACTIVE_LOW 1
// SWITCHER: held while the board starts, it boots the ROM switcher
// (BOARD_SD_SWITCHER_PATH) instead of Kickstart. Active low: a button to GND
// (internal pull-up). Active high (=0): a button module (VCC to 3V3, OUT
// with its own pull-down) that drives OUT high when pressed.
#ifndef BOARD_SWITCHER_BUTTON_PIN
#define BOARD_SWITCHER_BUTTON_PIN 26
#endif
#ifndef BOARD_SWITCHER_BUTTON_ACTIVE_LOW
#define BOARD_SWITCHER_BUTTON_ACTIVE_LOW 1
#endif

// ---------------------------------------------------------------------------
//  PSRAM  (QMI CS1)
// ---------------------------------------------------------------------------
// hardware_psram configures QMI at SDK startup using the CMake-provided
// PICO_PSRAM_CS_PIN.
#define BOARD_PSRAM_CS_PIN       PICO_PSRAM_CS_PIN
#define BOARD_PSRAM_BASE         0x11000000u // CS1 XIP window
#ifndef BOARD_PSRAM_SIZE_BYTES
#define BOARD_PSRAM_SIZE_BYTES   PICO_PSRAM_SIZE_BYTES
#endif

// ---------------------------------------------------------------------------
//  DVI / HDMI output   (differential pairs are {P = pin, N = pin+1})
// ---------------------------------------------------------------------------
// Raspberry Pi's Pico DVI Sock ordering on GPIO12..19, the RP2350 HSTX pins.
// src/dvi_display.c maps these pairs onto HSTX output bits 0..7.
#define BOARD_DVI_TMDS_D0_PIN    12    // pair 12/13  (blue/control)
#define BOARD_DVI_TMDS_D1_PIN    18    // pair 18/19  (green)
#define BOARD_DVI_TMDS_D2_PIN    16    // pair 16/17  (red)
#define BOARD_DVI_TMDS_CLK_PIN   14    // pair 14/15
#define BOARD_DVI_PIN_RANGE      "GPIO12..19"

// ---------------------------------------------------------------------------
//  microSD card (SPI), optional: build with -DOMEGA_ENABLE_SDCARD=ON
// ---------------------------------------------------------------------------
// The WeAct board has no slot; these defaults (SPI1, GPIO28..31, a Pmod TF
// card adapter's pins 3/1/4/2) must match the external module's wiring and
// can be overridden with -D definitions.
#ifndef BOARD_SD_SPI
#define BOARD_SD_SPI            spi1
#endif
#ifndef BOARD_SD_SCK_PIN
#define BOARD_SD_SCK_PIN        30    // SPI1 SCK
#endif
#ifndef BOARD_SD_MOSI_PIN
#define BOARD_SD_MOSI_PIN       31    // SPI1 TX   (SD CMD)
#endif
#ifndef BOARD_SD_MISO_PIN
#define BOARD_SD_MISO_PIN       28    // SPI1 RX   (SD DAT0)
#endif
#ifndef BOARD_SD_CS_PIN
#define BOARD_SD_CS_PIN         29    // plain GPIO (SD DAT3)
#endif
#ifndef BOARD_SD_SPI_BAUD
#define BOARD_SD_SPI_BAUD       12500000u   // 12.5 MHz after card init
#endif
#ifndef BOARD_SD_ROM_PATH
#define BOARD_SD_ROM_PATH       "0:/rom/kick13.rom"
#endif
#ifndef BOARD_SD_SWITCHER_PATH
#define BOARD_SD_SWITCHER_PATH  "0:/rom/switcher.rom"
#endif

// ---------------------------------------------------------------------------
//  Amiga memory map inside PSRAM   (emulator layout, not board wiring)
// ---------------------------------------------------------------------------
//  0x000000..0x1FFFFF  Chip RAM        (2 MB)      Amiga 0x000000..0x1FFFFF
//  0x200000..0x3FFFFF  Fast RAM        (2 MB)      Zorro II autoconfig
//                                                  board, Amiga 0x200000..
//  0x400000..0x4FFFFF  RTG VRAM        (1 MB)      the Picasso II+'s,
//                                                  Amiga 0x400000..
//  0x500000..0x57FFFF  SD Kickstart cache (512 KB, SD-card builds)
//  0x580000..0x583FFF  DF0 MFM track   (16 KB; the active track of the flash
//                                       ADF, or the empty drive's track,
//                                       read uncached; the ADF streams)
//  0x584000..0x67DFFF  Video DMA raster scratch (640x400 ARGB32, 1000 KB)
//  0x67E000..0x67FFFF  Free
//  0x680000..0x77FFFF  Framebuffer 640x400 ARGB32 (1 MB)
//  0x780000..0x7817FF  SD work area (FatFs volume, sector buffers, the
//                       KickSmash state and its settings text)
//  0x781800..0x781FFF  Free
//  0x782000..0x782FFF  RTG card's state (registers, DAC, blitter buffer)
//  0x783000..0x787FFF  Free
//  0x788000..0x7A7FFF  68000 opcode table scratch (128 KB, boot only)
//  0x7A8000..0x7CBFFF  HDMI frame records (dvi_indexed_frame_t x4, 144 KB)
//  0x7CC000..0x7FEFFF  Reserved
//  0x7FF000..0x7FF00B  DF0 disk rotation state (survives resets only)
#define BOARD_MAP_CHIPRAM_OFFSET   0x000000u
#define BOARD_MAP_FAST_RAM_OFFSET  0x200000u
#define BOARD_MAP_RTG_VRAM_OFFSET  0x400000u
#define BOARD_MAP_DF0_OFFSET       0x580000u
#define BOARD_MAP_FRAMEBUF_OFFSET  0x680000u
#define BOARD_MAP_DVI_RECORDS_OFFSET 0x7A8000u
#define BOARD_MAP_DVI_RECORDS_SIZE 0x024000u
#define BOARD_MAP_OPCODE_BUILD_OFFSET 0x788000u
#define BOARD_MAP_BOOT_STATE_OFFSET 0x7FF000u
#define BOARD_MAP_VIDEO_RASTER_OFFSET 0x584000u
#define BOARD_MAP_SD_ROM_OFFSET    0x500000u
#define BOARD_MAP_SD_WORK_OFFSET   0x780000u
#define BOARD_MAP_RTG_STATE_OFFSET 0x782000u

#define BOARD_MAP_CHIPRAM_SIZE     0x200000u   // 2 MB
// The Zorro II boards (CMake OMEGA_FAST_RAM and OMEGA_RTG, both on by
// default). Fast RAM: a power of two from 64 KB to 2 MB, or 0 for none.
#ifndef OMEGA_FAST_RAM
#define OMEGA_FAST_RAM 1
#endif
#ifndef OMEGA_RTG
#define OMEGA_RTG 0
#endif
#if OMEGA_RTG && !OMEGA_FAST_RAM
#error "OMEGA_RTG needs OMEGA_FAST_RAM: the VRAM follows the Fast RAM"
#endif
#define BOARD_MAP_FAST_RAM_SIZE    (OMEGA_FAST_RAM ? 0x200000u : 0u)   // 2 MB
// RTG: the Picasso II+'s VRAM, 1 MB (2 MB has no room while the ARGB
// fallback buffers exist). It needs the 2 MB of Fast RAM before it:
// autoconfig then puts it at 0x400000, which Memory.c maps 1:1 onto this
// PSRAM. Its PSRAM stays reserved without the card.
#define BOARD_MAP_RTG_VRAM_SIZE    0x100000u   // 1 MB
#define BOARD_MAP_FLOPPY_SIZE      0x004000u   // 16 KB (one DF0 MFM track)
#define BOARD_MAP_SD_ROM_SIZE      0x080000u   // 512 KB maximum
#define BOARD_MAP_SD_WORK_SIZE     0x001800u   // 6 KB

//  Kickstart ROM in flash (place with tools/combine_uf2.py at offset 0x200000)
#define BOARD_ROM_FLASH_BASE       0x10200000u
#define BOARD_ROM_SIZE             0x080000u   // 512 KB (256 KB ROMs mirrored)

// ---------------------------------------------------------------------------
//  Compile-time sanity checks
// ---------------------------------------------------------------------------
_Static_assert(BOARD_MAP_FAST_RAM_OFFSET + BOARD_MAP_FAST_RAM_SIZE <=
               BOARD_MAP_RTG_VRAM_OFFSET, "Fast RAM overruns the RTG VRAM");
_Static_assert(BOARD_MAP_RTG_VRAM_OFFSET + BOARD_MAP_RTG_VRAM_SIZE <=
               BOARD_MAP_SD_ROM_OFFSET, "RTG VRAM overruns the SD ROM cache");
_Static_assert(BOARD_MAP_SD_ROM_OFFSET + BOARD_MAP_SD_ROM_SIZE <=
               BOARD_MAP_DF0_OFFSET, "SD ROM cache overruns the DF0 track");
_Static_assert(BOARD_MAP_DF0_OFFSET + BOARD_MAP_FLOPPY_SIZE <=
               BOARD_MAP_VIDEO_RASTER_OFFSET, "DF0 track overruns the raster");
_Static_assert(BOARD_MAP_FRAMEBUF_OFFSET + 640u*400u*4u <= BOARD_PSRAM_SIZE_BYTES,
               "framebuffer overruns fitted PSRAM - shrink the map or fit more PSRAM");
_Static_assert(BOARD_MAP_VIDEO_RASTER_OFFSET + 640u*400u*4u <= BOARD_MAP_FRAMEBUF_OFFSET,
               "video raster scratch buffer overruns fitted PSRAM");
