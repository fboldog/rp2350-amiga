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
// The WeAct board has no slot; these defaults (SPI1) must match the external
// module's wiring and can be overridden with -D definitions.
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
#define BOARD_SD_MISO_PIN       40    // SPI1 RX   (SD DAT0)
#endif
#ifndef BOARD_SD_CS_PIN
#define BOARD_SD_CS_PIN         43    // plain GPIO (SD DAT3)
#endif
#ifndef BOARD_SD_SPI_BAUD
#define BOARD_SD_SPI_BAUD       12500000u   // 12.5 MHz after card init
#endif
#ifndef BOARD_SD_ROM_PATH
#define BOARD_SD_ROM_PATH       "0:/rom/kick13.rom"
#endif

// ---------------------------------------------------------------------------
//  Amiga memory map inside PSRAM   (emulator layout, not board wiring)
// ---------------------------------------------------------------------------
//  0x000000..0x1FFFFF  Chip RAM        (2 MB)      Amiga 0x000000..0x1FFFFF
//  0x200000..0x27FFFF  Slow/Ranger RAM (512 KB)    Amiga 0xC00000..0xC7FFFF
//  0x280000..0x47FFFF  DF0 MFM buffer  (2 MB; empty-drive image when no ADF
//                                       is in flash, else the active track,
//                                       read uncached; the ADF streams)
//  0x480000..0x4FFFFF  SD Kickstart cache (512 KB, SD-card builds)
//  0x540000..0x639FFF  Video DMA raster scratch (640x400 ARGB32, 1000 KB)
//  0x680000..0x77FFFF  Framebuffer 640x400 ARGB32 (1 MB)
//  0x780000..0x787FFF  HDMI frame records (dvi_indexed_frame_t x2, 32 KB)
//  0x788000..0x7A7FFF  68000 opcode table scratch (128 KB, boot only)
//  0x7A8000..0x7FEFFF  Reserved
//  0x7FF000..0x7FF00B  DF0 disk rotation state (survives resets only)
#define BOARD_MAP_CHIPRAM_OFFSET   0x000000u
#define BOARD_MAP_SLOWRAM_OFFSET   0x200000u
#define BOARD_MAP_DF0_OFFSET       0x280000u
#define BOARD_MAP_FRAMEBUF_OFFSET  0x680000u
#define BOARD_MAP_DVI_RECORDS_OFFSET 0x780000u
#define BOARD_MAP_DVI_RECORDS_SIZE 0x008000u
#define BOARD_MAP_OPCODE_BUILD_OFFSET 0x788000u
#define BOARD_MAP_BOOT_STATE_OFFSET 0x7FF000u
#define BOARD_MAP_VIDEO_RASTER_OFFSET 0x540000u
#define BOARD_MAP_SD_ROM_OFFSET    0x480000u

#define BOARD_MAP_CHIPRAM_SIZE     0x200000u   // 2 MB
#define BOARD_MAP_SLOWRAM_SIZE     0x080000u   // 512 KB
#define BOARD_MAP_FLOPPY_SIZE      0x200000u   // 2 MB (DF0)
#define BOARD_MAP_SD_ROM_SIZE      0x080000u   // 512 KB maximum

//  Kickstart ROM in flash (place with tools/combine_uf2.py at offset 0x200000)
#define BOARD_ROM_FLASH_BASE       0x10200000u
#define BOARD_ROM_SIZE             0x080000u   // 512 KB (256 KB ROMs mirrored)

// ---------------------------------------------------------------------------
//  Compile-time sanity checks
// ---------------------------------------------------------------------------
_Static_assert(BOARD_MAP_FRAMEBUF_OFFSET + 640u*400u*4u <= BOARD_PSRAM_SIZE_BYTES,
               "framebuffer overruns fitted PSRAM - shrink the map or fit more PSRAM");
_Static_assert(BOARD_MAP_VIDEO_RASTER_OFFSET + 640u*400u*4u <= BOARD_MAP_FRAMEBUF_OFFSET,
               "video raster scratch buffer overruns fitted PSRAM");
