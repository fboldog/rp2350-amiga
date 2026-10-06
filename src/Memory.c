// PSRAM-backed Amiga memory for the RP2350 port of Omega.
// Replaces the original Memory.c (which used a 16 MB static array).
//
// Address translation:
//   Amiga 0x000000–0x1FFFFF → PSRAM[0x000000] (chip RAM, 2 MB)
//   0xC00000–0xD7FFFF       → custom register mirror (no slow RAM, as an
//                             A500 without trapdoor RAM)
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

// ── ROM (read-only, live in flash via XIP) ────────────────────────────────
static const uint8_t *rom_base;
static uint32_t       rom_size;

static void psram_clear_words(uint8_t *memory, uint32_t bytes) {
    memset(memory, 0, bytes);
}

// ── Helpers ───────────────────────────────────────────────────────────────

// Big-endian word stored in RAM ↔ little-endian ARM read/write helpers.
// One (possibly unaligned) load or store plus REV/REV16 instead of separate
// byte accesses; Cortex-M33 permits unaligned access to XIP/PSRAM and SRAM.
static inline uint16_t ram_read_word(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return __builtin_bswap16(v);
}
static inline uint32_t ram_read_long(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return __builtin_bswap32(v);
}
static inline void ram_write_word(uint8_t *p, uint16_t v) {
    v = __builtin_bswap16(v);
    memcpy(p, &v, sizeof(v));
}
static inline void ram_write_long(uint8_t *p, uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(p, &v, sizeof(v));
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

// ── Page table for instruction fetches ───────────────────────────────────
// One entry per 64 KB page of the 24-bit 68000 address space: the host
// address of the page's first byte where the page is plain memory (chip
// RAM, ROM with its 256 KB mirror), NULL elsewhere (registers, CIAs, Gayle,
// unmapped). An instruction fetch then costs one lookup wherever the code
// runs. Data accesses keep their range checks: a lookup in front of them
// slowed register accesses (idle screens -1 %) without helping.
#define PAGE_SHIFT 16
#define PAGE_COUNT (0x1000000u >> PAGE_SHIFT)
const uint8_t *chip_fetch_page[PAGE_COUNT];

static void memory_map_pages(void) {
    for (uint32_t p = 0; p < PAGE_COUNT; ++p) {
        const uint32_t a = p << PAGE_SHIFT;
        const uint8_t *mem = NULL;
        if (a <= CHIPTOP)
            mem = chip_ram + a;
        else if (a >= 0xF80000u && rom_base)
            mem = rom_base + rom_addr(a);
        chip_fetch_page[p] = mem;
    }
}

// Host pointer for a 68000 address in plain memory, or NULL.
static inline const uint8_t *fetch_ptr(uint32_t address) {
    if (address >= 0x1000000u)
        return NULL;
    const uint8_t *page = chip_fetch_page[address >> PAGE_SHIFT];
    return page ? page + (address & ((1u << PAGE_SHIFT) - 1)) : NULL;
}

// ── 0xC00000–0xD7FFFF: custom register mirror ───────────────────────────
// There is no slow (trapdoor/Ranger) RAM: 2 MB chip RAM is enough. As on an
// A500 without it, the custom chips answer in this range with their
// registers repeated every 512 bytes; Kickstart's memory sizing tells the
// mirror from RAM by writing INTENA through it and reading INTENAR.
#define MIRROR_START 0xC00000u
#define MIRROR_END   0xD80000u
static inline int in_custom_mirror(uint32_t address) {
    return address >= MIRROR_START && address < MIRROR_END;
}
static inline uint32_t custom_mirror(uint32_t address) {
    return 0xDFF000u | (address & 0x1FFu);
}

// ── Public API ────────────────────────────────────────────────────────────

void memory_init(void) {
    chip_ram  = psram_ptr(PSRAM_CHIPRAM_OFFSET);

    // ROM lives in flash; the user must place the Kickstart binary at
    // flash offset 0x200000 (i.e. absolute address ROM_FLASH_BASE).
    rom_base = (const uint8_t *)ROM_FLASH_BASE;
    rom_size = ROM_SIZE;

    // Validate ROM presence: first byte of a valid Kickstart is 0x11
    if (rom_base[0] != 0x11) {
        // ROM not found – ROM region will return 0 / open bus
        rom_base = NULL;
        rom_size = 0;
        printf("ROM: no Kickstart header at flash 0x%08x\n",
               (unsigned)ROM_FLASH_BASE);
    } else {
        uint16_t opcode = ram_read_word(rom_base + 2);
        uint32_t entry = ram_read_long(rom_base + 4);
        printf("ROM: flash 0x%08x, header=%02x%02x opcode=%04x entry=%08lx\n",
               (unsigned)ROM_FLASH_BASE, rom_base[0], rom_base[1], opcode,
               (unsigned long)entry);
    }

    memory_map_pages();

    printf("Memory: clearing chip RAM\n");
    memory_clear_chipram();
    printf("Memory: PSRAM clear complete\n");
}

int memory_set_rom(const uint8_t *data, uint32_t size) {
    if (!data || (size != 0x40000u && size != 0x80000u) || data[0] != 0x11) {
        return 0;
    }
    rom_base = data;
    rom_size = size;
    memory_map_pages();
    return 1;
}

void memory_clear_chipram(void) {
    // Leave longword at 0x000000 (Guru indicator) intact; clear from 4 up
    psram_clear_words(chip_ram + 4, PSRAM_CHIPRAM_SIZE - 4);
}

// ── chipReadByte ──────────────────────────────────────────────────────────
unsigned int chipReadByte(unsigned int address) {
    // Chip RAM is by far the most common target; test it first.
    if (address <= CHIPTOP) return chip_ram[address];
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
        if (off >= 32u) return 0;
        return getChipReg8[off]();
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) return readGayleB(address);
    // Custom register mirror (no slow RAM)
    if (address > 0xBFFFFFu)
        return in_custom_mirror(address) ? chipReadByte(custom_mirror(address)) : 0;
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
    if (address <= CHIPTOP) return ram_read_word(&chip_ram[address]);
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
    // Custom register mirror (no slow RAM)
    if (address > 0xBFFFFFu)
        return in_custom_mirror(address) ? chipReadWord(custom_mirror(address)) : 0;
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
    if (address <= CHIPTOP) return ram_read_long(&chip_ram[address]);
    // ROM
    if (address > 0xF7FFFFu) {
        if (!rom_base) return 0;
        return ram_read_long(rom_ptr(address));
    }
    // Custom chipset registers (32-bit = two 16-bit reads)
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        // getChipReg32 covers only the first 16 registers; everything else
        // (including autoconfig space) reads 0 like its noReadL entries.
        if (off >= 16u) return 0;
        debugChipAddress = off;
        return getChipReg32[off]();
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) return readGayleL(address);
    // Custom register mirror (no slow RAM)
    if (address > 0xBFFFFFu)
        return in_custom_mirror(address) ? chipReadLong(custom_mirror(address)) : 0;
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
    if (address <= CHIPTOP) { chip_ram[address] = (uint8_t)value; return; }
    if (address >= 0xF80000u) return;  // ROM: no-op
    // Custom chipset: the 68000 drives a byte on both halves of the data
    // bus and the custom chips latch the whole word, so a byte write sets
    // the register to the byte twice (DiagROM's raster test writes COLOR00's
    // low byte: 0x47 -> 0x4747, colour 0x747).
    if (address > 0xDFEFFFu) {
        chipWriteWord(address & ~1u, (value & 0xFFu) * 0x0101u);
        return;
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) { writeGayleB(address, value); return; }
    // Custom register mirror (no slow RAM)
    if (address > 0xBFFFFFu) {
        if (in_custom_mirror(address))
            chipWriteByte(custom_mirror(address), value);
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
    if (address <= CHIPTOP) {
        ram_write_word(&chip_ram[address], (uint16_t)value);
        return;
    }
    if (address >= 0xF80000u) return;
    // Custom chipset registers
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        // The tables cover 0xDFF000..0xDFF1FF. A runaway 68k can write
        // anywhere up to the ROM; ignore it rather than jump through a
        // pointer read past the table.
        if (off >= 256u) return;
        debugChipAddress = off;
        debugChipValue   = value;
        dmaIdleCacheValid = 0;
        putChipReg16[off]((uint16_t)value);
        return;
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) { writeGayle(address, value); return; }
    // Custom register mirror (no slow RAM)
    if (address > 0xBFFFFFu) {
        if (in_custom_mirror(address))
            chipWriteWord(custom_mirror(address), value);
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
    if (address <= CHIPTOP) {
        ram_write_long(&chip_ram[address], value);
        return;
    }
    if (address >= 0xF80000u) return;
    // Custom chipset registers
    if (address > 0xDFEFFFu) {
        uint32_t off = (address - 0xDFF000u) >> 1;
        if (off >= 256u) return;    // see chipWriteWord
        debugChipAddress = off;
        dmaIdleCacheValid = 0;
        putChipReg32[off](value);
        return;
    }
    // Gayle/IDE
    if (address > 0xD9FFFFu) { writeGayleL(address, value); return; }
    // Custom register mirror (no slow RAM)
    if (address > 0xBFFFFFu) {
        if (in_custom_mirror(address))
            chipWriteLong(custom_mirror(address), value);
        return;
    }
    // CIA: no long writes
    if (address >= 0xBFD000u) return;
    // 24-bit fast RAM area (not present)
    if (address > 0x1FFFFFu) return;
    // Chip RAM
    address &= CHIPTOP;
    ram_write_long(&chip_ram[address], value);
}

// ── Instruction fetch (Musashi prefetch) ─────────────────────────────────
// Code runs from ROM or chip RAM: one page-table lookup; anything
// else takes the general data path.
unsigned int chipFetchLong(unsigned int address) {
    const uint8_t *p = fetch_ptr(address);
    if (p)
        return ram_read_long(p);
    return chipReadLong(address);
}

unsigned int chipFetchWord(unsigned int address) {
    const uint8_t *p = fetch_ptr(address);
    if (p)
        return ram_read_word(p);
    return chipReadWord(address);
}
