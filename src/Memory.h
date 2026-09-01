// Memory interface for the RP2350 PSRAM port of Omega.
// Provides the same API as the original Memory.h so all Omega source files
// compile unchanged.  The backing store is 8 MB PSRAM instead of a 16 MB
// static array.
//
// Amiga address-to-PSRAM mapping
//   0x000000–0x1FFFFF  → PSRAM chip RAM   (2 MB)
//   0xC00000–0xDFEFFF  → PSRAM slow RAM   (512 KB in PSRAM[0x200000–0x27FFFF])
//   0xF80000–0xFFFFFF  → Flash ROM        (read-only, XIP at ROM_FLASH_BASE)
//
// Everything else returns 0 / is silently discarded on writes.

#pragma once
#include <stdint.h>

// ── chip RAM size ─────────────────────────────────────────────────────────
#define CHIPTOP 0x1FFFFFu   // 2 MB chip RAM top

// ── ROM source ────────────────────────────────────────────────────────────
// Kickstart ROM is stored in flash starting at this absolute address.
// Place the ROM file at flash offset 0x200000 (2 MB from flash start) in
// your CMakeLists.txt / UF2 combined image so it lands at 0x10200000.
#define ROM_FLASH_BASE  0x10200000u
#define ROM_SIZE        0x080000u   // 512 KB max (256 KB ROMs are mirrored)

// ── Direct chip RAM access helpers (used by Chipset.c and DMA.c) ─────────
// On desktop, chip RAM is the start of the low16Meg array.
// On RP2350, it is memory-mapped PSRAM.
#ifdef PICO_BUILD
#include "psram.h"
// Byte pointer to the start of chip RAM (PSRAM-backed, memory-mapped)
#define CHIPRAM_BASE_PTR  ((uint8_t*)(PSRAM_BASE + PSRAM_CHIPRAM_OFFSET))
#else
// low16Meg is declared by the desktop Memory.c
extern unsigned char low16Meg[16777216];
#define CHIPRAM_BASE_PTR  ((uint8_t*)low16Meg)
#endif

// ── public API ───────────────────────────────────────────────────────────
// Called once at startup; clears chip/slow RAM and mirrors ROM if needed.
void memory_init(void);
// Wipe chip RAM from address 4 upward (called on 68k RESET)
void memory_clear_chipram(void);

// Raw Amiga address read/write used by the CPU and DMA
unsigned int chipReadByte(unsigned int address);
unsigned int chipReadWord(unsigned int address);
unsigned int chipReadLong(unsigned int address);

void chipWriteByte(unsigned int address, unsigned int value);
void chipWriteWord(unsigned int address, unsigned int value);
void chipWriteLong(unsigned int address, unsigned int value);
