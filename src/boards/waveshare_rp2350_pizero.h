/*
 * Pico SDK board header for the Waveshare RP2350-PiZero.
 *
 * Based on the header Waveshare ships in its RP2350-PiZero demo pack, with the
 * on-board QSPI PSRAM chip-select added.  Selected from CMakeLists.txt via
 *   set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards)
 *   set(PICO_BOARD waveshare_rp2350_pizero)
 *
 * NOTE: this only covers what the Pico SDK build needs (variant select, flash,
 * default UART / LED / I2C, PSRAM CS).  The full board pin map for this project
 * (DVI, microSD, USB, PSRAM geometry) lives in src/board_config.h.
 *
 * -----------------------------------------------------------------------------
 * THIS HEADER IS ALSO INCLUDED BY THE ASSEMBLER - PREPROCESSOR DIRECTIVES ONLY.
 * -----------------------------------------------------------------------------
 */

// pico_cmake_set PICO_PLATFORM=rp2350

#ifndef _BOARDS_WAVESHARE_RP2350_PIZERO_H
#define _BOARDS_WAVESHARE_RP2350_PIZERO_H

// For board detection
#define WAVESHARE_RP2350_PIZERO

// --- RP2350 VARIANT --------------------------------------------------------
// The PiZero uses the 80-pin RP2350B (GPIO0..47).  0 => "not the A variant".
#define PICO_RP2350A 0

// --- UART (debug console) ------------------------------------------------
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---------------------------------------------------------------
// No plain LED; there is a single WS2812 (NeoPixel) on GPIO2.
#ifndef PICO_DEFAULT_WS2812_PIN
#define PICO_DEFAULT_WS2812_PIN 2
#endif

// --- I2C (also the HDMI DDC bus after level shifting) ------------------
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 6
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif

// --- FLASH -----------------------------------------------------------
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (16 * 1024 * 1024)
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// --- PSRAM ---------------------------------------------------------------
// The PiZero has a *footprint* (U1) for an 8-pin QSPI PSRAM on the shared QMI
// bus (QSPI_SD0..3 / QSPI_SCLK), with its own chip-select on GPIO47.  Not
// fitted from the factory - see src/board_config.h (BOARD_HAS_PSRAM).
#ifndef PICO_RP2350_PSRAM_CS_PIN
#define PICO_RP2350_PSRAM_CS_PIN 47
#endif

#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif /* _BOARDS_WAVESHARE_RP2350_PIZERO_H */
