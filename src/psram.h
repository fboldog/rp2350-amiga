// PSRAM layout definitions for RP2350 Pimoroni Pico Plus 2
// APS6404L-3SQR: 8MB QSPI PSRAM on QMI CS1
//
// After boot-time QMI init, PSRAM is memory-mapped at PSRAM_BASE.
// Pimoroni Pico Plus 2 board config sets this up automatically.

#pragma once
#include <stdint.h>
#include "pico/stdlib.h"

// Address where PSRAM is mapped in RP2350 address space (CS1 window)
#define PSRAM_BASE      0x11000000u
#define PSRAM_SIZE      0x800000u   // 8MB

// ── Amiga memory layout inside PSRAM ──────────────────────────────────────
// 0x000000–0x1FFFFF  Chip RAM (2MB)
// 0x200000–0x27FFFF  Slow/Ranger RAM (512KB)
// 0x280000–0x47FFFF  DF0 MFM buffer (2MB)
// 0x480000–0x67FFFF  DF1 MFM buffer (2MB)
// 0x680000–0x77FFFF  Framebuffer 640×400×4 bytes (1MB)
// 0x780000–0x7FFFFF  Reserved / scratch (512KB)

#define PSRAM_CHIPRAM_OFFSET    0x000000u
#define PSRAM_SLOWRAM_OFFSET    0x200000u
#define PSRAM_DF0_OFFSET        0x280000u
#define PSRAM_DF1_OFFSET        0x480000u
#define PSRAM_FRAMEBUF_OFFSET   0x680000u

#define PSRAM_CHIPRAM_SIZE      0x200000u   // 2MB
#define PSRAM_SLOWRAM_SIZE      0x080000u   // 512KB
#define PSRAM_FLOPPY_SIZE       0x200000u   // 2MB per drive

static inline uint8_t *psram_ptr(uint32_t offset) {
    return (uint8_t *)(PSRAM_BASE + offset);
}

// Initialise QMI CS1 for APS6404L PSRAM at up to 150 MHz.
// Call once early in main() before any PSRAM access.
void psram_init(void);
