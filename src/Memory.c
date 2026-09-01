// PSRAM-backed Amiga memory for the RP2350 port of Omega.
// Replaces the original Memory.c (which used a 16 MB static array).
//
// Address translation:
//   Amiga 0x000000–0x1FFFFF → PSRAM[0x000000] (chip RAM, 2 MB)
//   Amiga 0xC00000–0xDFEFFF → PSRAM[0x200000] (slow RAM, ≤512 KB)
//   Amiga 0xF80000–0xFFFFFF → Flash ROM at ROM_FLASH_BASE (read-only)
//   0xBFD000–0xBFFFFF       → CIA (handled via CIARead/CIAWrite)
//   0xDFF000–0xDFF1FF       → Custom chipset registers
//   0xDA0000–0xDAFFFF       → Gayle IDE (Amiga 600/1200)
//   Everything else         → open bus (reads 0, writes dropped)

#include "Memory.h"
#include "psram.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/debug.h"
#include "../omega/DMA.h"
#include "../omega/Gayle.h"
#include <string.h>
#include <stdint.h>

// ── PSRAM region base pointers ────────────────────────────────────────────
static uint8_t *chip_ram;   // PSRAM + PSRAM_CHIPRAM_OFFSET
static uint8_t *slow_ram;   // PSRAM + PSRAM_SLOWRAM_OFFSET

// ── ROM (read-only, live in flash via XIP) ────────────────────────────────
static const uint8_t *rom_base;
static uint32_t       rom_size;

// ── Helpers ───────────────────────────────────────────────────────────────

// Big-endian word stored in RAM ↔ little-endian ARM read/write helpers
static inline uint16_t ram_read_word(const uint8_t *p) {
    return (uint16_t)(p[0] << 8) | p[1];
}
static inline uint32_t ram_read_long(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static inline void ram_write_word(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void ram_write_long(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

// ── ROM helper: 256 KB ROM mirrored to fill 512 KB slot ──────────────────
static inline uint32_t rom_addr(uint32_t amiga_addr) {
    uint32_t offset = amiga_addr - 0xF80000u;
    // If only a 256 KB ROM (0x40000), mirror it twice
    if (rom_size <= 0x40000u) offset &= 0x3FFFFu;
    return offset;
}
static inline const uint8_t *rom_ptr(uint32_t amiga_addr) {
    return rom_base + rom_addr(amiga_addr);
}

// ── Slow RAM address to PSRAM pointer ────────────────────────────────────
static inline uint8_t *slow_ptr(uint32_t amiga_addr) {
    uint32_t offset = amiga_addr - 0xC00000u;
    if (offset >= PSRAM_SLOWRAM_SIZE) return NULL;
    return slow_ram + offset;
}

// ── Public API ────────────────────────────────────────────────────────────

void memory_init(void) {
    chip_ram  = psram_ptr(PSRAM_CHIPRAM_OFFSET);
    slow_ram  = psram_ptr(PSRAM_SLOWRAM_OFFSET);

    // ROM lives in flash; the user must place the Kickstart binary at
    // flash offset 0x200000 (i.e. absolute address ROM_FLASH_BASE).
    rom_base = (const uint8_t *)ROM_FLASH_BASE;
    rom_size = ROM_SIZE;

    // Validate ROM presence: first byte of a valid Kickstart is 0x11
    if (rom_base[0] != 0x11) {
        // ROM not found – ROM region will return 0 / open bus
        rom_base = NULL;
        rom_size = 0;
    }

    memory_clear_chipram();
    memset(slow_ram, 0, PSRAM_SLOWRAM_SIZE);
}

void memory_clear_chipram(void) {
    // Leave longword at 0x000000 (Guru indicator) intact; clear from 4 up
    memset(chip_ram + 4, 0, PSRAM_CHIPRAM_SIZE - 4);
}

// ── chipReadByte ──────────────────────────────────────────────────────────
unsigned int chipReadByte(unsigned int address) {
    // ROM
    if (address >= 0xF80000u) {
        if (!rom_base) return 0;
        return rom_ptr(address)[0];
    }
    // Autoconfig / Z2 space above chipset
    if (address > 0xDFFFFFu) return 0;
    // Custom chipset registers
    if (address > 0xDFEFFFu) {
        uint32_t off = address - 0xDFF000u;
        debugChipAddress = off;
        return getChipReg8[off]();
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) return readGayleB(address);
    // Slow RAM
    if (address > 0xBFFFFFu) {
        uint8_t *p = slow_ptr(address);
        return p ? *p : 0;
    }
    // CIA A
    if (address >= 0xBFE001u) return CIARead(&CIAA, (address - 0xBFE001u) >> 8);
    // CIA B
    if (address >= 0xBFD000u) return CIARead(&CIAB, (address - 0xBFD000u) >> 8);
    // Chip RAM (2 MB, address wraps)
    address &= CHIPTOP;
    return chip_ram[address];
}

// ── chipReadWord ─────────────────────────────────────────────────────────
unsigned int chipReadWord(unsigned int address) {
    // ROM
    if (address > 0xF7FFFFu) {
        if (!rom_base) return 0;
        return ram_read_word(rom_ptr(address));
    }
    // Autoconfig space
    if (address > 0xDFFFFFu) return 0;
    // Custom chipset registers
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        // Only a handful of registers are readable; DeniseID is the fallback
        if (off > 16) {
            if (off == 62) return chipset.deniseid;
            debugChipAddress = off;
            return chipset.deniseid;
        }
        debugChipAddress = off;
        return getChipReg16[off]();
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) return readGayle(address);
    // Slow RAM
    if (address > 0xBFFFFFu) {
        uint8_t *p = slow_ptr(address);
        return p ? ram_read_word(p) : 0;
    }
    // CIA A/B
    if (address >= 0xBFE001u) return CIARead(&CIAA, (address - 0xBFE001u) >> 8);
    if (address >= 0xBFD000u) return CIARead(&CIAB, (address - 0xBFD000u) >> 8);
    // 24-bit expansion fast RAM (not present → open bus)
    if (address > 0x1FFFFFu) return 0;
    // Chip RAM
    address &= CHIPTOP;
    return ram_read_word(&chip_ram[address]);
}

// ── chipReadLong ─────────────────────────────────────────────────────────
unsigned int chipReadLong(unsigned int address) {
    // ROM
    if (address > 0xF7FFFFu) {
        if (!rom_base) return 0;
        return ram_read_long(rom_ptr(address));
    }
    // Custom chipset registers (32-bit = two 16-bit reads)
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        off &= 0xFFu;
        debugChipAddress = off;
        return getChipReg32[off]();
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) return readGayleL(address);
    // Slow RAM
    if (address > 0xBFFFFFu) {
        uint8_t *p = slow_ptr(address);
        return p ? ram_read_long(p) : 0;
    }
    // CIA A/B
    if (address >= 0xBFE001u) return CIARead(&CIAA, (address - 0xBFE001u) >> 8);
    if (address >= 0xBFD000u) return CIARead(&CIAB, (address - 0xBFD000u) >> 8);
    // 24-bit fast RAM
    if (address > 0x1FFFFFu) return 0;
    // Chip RAM
    address &= CHIPTOP;
    return ram_read_long(&chip_ram[address]);
}

// ── chipWriteByte ─────────────────────────────────────────────────────────
void chipWriteByte(unsigned int address, unsigned int value) {
    if (address >= 0xF80000u) return;  // ROM: no-op
    // Custom chipset (byte writes uncommon, ignore)
    if (address > 0xDFEFFFu) return;
    // Gayle/IDE
    if (address > 0xD9FFFFu) { writeGayleB(address, value); return; }
    // Slow RAM
    if (address > 0xBFFFFFu) {
        uint8_t *p = slow_ptr(address);
        if (p) *p = (uint8_t)value;
        return;
    }
    // CIA A
    if (address >= 0xBFE001u) { CIAWrite(&CIAA, (address - 0xBFE001u) >> 8, (uint8_t)value); return; }
    // CIA B
    if (address >= 0xBFD000u) { CIAWrite(&CIAB, (address - 0xBFD000u) >> 8, (uint8_t)value); return; }
    // Alternative CIA A mirror seen in KS3
    if (address >= 0xBFA001u) { CIAWrite(&CIAA, (address - 0xBFA001u) >> 8, (uint8_t)value); return; }
    // Chip RAM
    if (address > 0x1FFFFFu) return;
    address &= CHIPTOP;
    chip_ram[address] = (uint8_t)value;
}

// ── chipWriteWord ─────────────────────────────────────────────────────────
void chipWriteWord(unsigned int address, unsigned int value) {
    if (address >= 0xF80000u) return;
    // Custom chipset registers
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        debugChipAddress = off;
        debugChipValue   = value;
        putChipReg16[off]((uint16_t)value);
        return;
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) { writeGayle(address, value); return; }
    // Slow RAM
    if (address > 0xBFFFFFu) {
        uint8_t *p = slow_ptr(address);
        if (p) ram_write_word(p, (uint16_t)value);
        return;
    }
    // CIA: word writes to CIA space are no-ops (byte-wide chips)
    if (address >= 0xBFD000u) return;
    // Chip RAM
    if (address > 0x1FFFFFu) return;
    address &= CHIPTOP;
    ram_write_word(&chip_ram[address], (uint16_t)value);
}

// ── chipWriteLong ─────────────────────────────────────────────────────────
void chipWriteLong(unsigned int address, unsigned int value) {
    if (address >= 0xF80000u) return;
    // Custom chipset registers
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        debugChipAddress = off;
        putChipReg32[off](value);
        return;
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) { writeGayleL(address, value); return; }
    // Slow RAM
    if (address > 0xBFFFFFu) {
        uint8_t *p = slow_ptr(address);
        if (p) ram_write_long(p, value);
        return;
    }
    // CIA: no long writes
    if (address >= 0xBFD000u) return;
    // 24-bit fast RAM area
    if (address > 0x1FFFFFu) {
        // Write through to PSRAM slow region as fast RAM (optional)
        return;
    }
    // Chip RAM
    address &= CHIPTOP;
    ram_write_long(&chip_ram[address], value);
}
