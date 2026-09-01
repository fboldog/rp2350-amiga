// PSRAM layout for the RP2350 port of Omega.
//
// Board wiring (CS pin, base address, fitted size, QMI timing) comes from
// board_config.h.  The Amiga-side memory map is defined there too; the names
// below are kept as short aliases so the rest of the code is unchanged.
//
// After boot-time QMI init (psram_init()), PSRAM is memory-mapped at PSRAM_BASE.

#pragma once
#include <stdint.h>
#include "pico/stdlib.h"
#include "board_config.h"

// Address where PSRAM is mapped in the RP2350 address space (QMI CS1 window)
#define PSRAM_BASE      BOARD_PSRAM_BASE
#define PSRAM_SIZE      BOARD_PSRAM_SIZE_BYTES

// ── Amiga memory layout inside PSRAM (see board_config.h for the table) ───
#define PSRAM_CHIPRAM_OFFSET    BOARD_MAP_CHIPRAM_OFFSET
#define PSRAM_SLOWRAM_OFFSET    BOARD_MAP_SLOWRAM_OFFSET
#define PSRAM_DF0_OFFSET        BOARD_MAP_DF0_OFFSET
#define PSRAM_DF1_OFFSET        BOARD_MAP_DF1_OFFSET
#define PSRAM_FRAMEBUF_OFFSET   BOARD_MAP_FRAMEBUF_OFFSET

#define PSRAM_CHIPRAM_SIZE      BOARD_MAP_CHIPRAM_SIZE
#define PSRAM_SLOWRAM_SIZE      BOARD_MAP_SLOWRAM_SIZE
#define PSRAM_FLOPPY_SIZE       BOARD_MAP_FLOPPY_SIZE

static inline uint8_t *psram_ptr(uint32_t offset) {
    return (uint8_t *)(PSRAM_BASE + offset);
}

// Initialise QMI CS1 for the fitted QSPI PSRAM (APS6404L class) at up to
// sys/BOARD_PSRAM_CLKDIV.  Call once early in main() before any PSRAM access.
void psram_init(void);
