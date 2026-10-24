// Boot-time setup of the Amiga memory map (Memory.c). Kept apart from
// Memory.c, whose code is copied to SRAM (src/ld/default_text_excludes.incl):
// this runs once, so it stays in flash.

#include "Memory.h"
#include "psram.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// Memory.c's state.
extern uint8_t *chip_ram;
extern const uint8_t *rom_base;
extern uint32_t rom_size;
extern const uint8_t *chip_fetch_page[];

#define PAGE_SHIFT 16
#define PAGE_COUNT (0x1000000u >> PAGE_SHIFT)

// One entry per 64 KB page: chip RAM, the ROM with its 256 KB mirror, or
// NULL (see fetch_ptr() in Memory.c).
static void memory_map_pages(void) {
    for (uint32_t p = 0; p < PAGE_COUNT; ++p) {
        const uint32_t a = p << PAGE_SHIFT;
        const uint8_t *mem = NULL;
        if (a <= CHIPTOP) {
            mem = chip_ram + a;
        } else if (a >= 0xF80000u && rom_base) {
            uint32_t offset = a - 0xF80000u;
            if (rom_size <= 0x40000u) offset &= 0x3FFFFu;
            mem = rom_base + offset;
        }
        chip_fetch_page[p] = mem;
    }
}

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
        uint16_t opcode = (uint16_t)(rom_base[2] << 8 | rom_base[3]);
        uint32_t entry = (uint32_t)rom_base[4] << 24 |
                         (uint32_t)rom_base[5] << 16 |
                         (uint32_t)rom_base[6] << 8 | rom_base[7];
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
    memset(chip_ram + 4, 0, PSRAM_CHIPRAM_SIZE - 4);
}
