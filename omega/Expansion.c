//
//  Expansion.c
//  Omega
//
//  Zorro II autoconfig (see Expansion.h). The board describes itself in
//  nibbles: each register byte is read as two words, its high nibble at
//  offset 4n and its low nibble at 4n+2, in data bits 15-12. All registers
//  but er_Type (0x00) and the control registers (0x40 up) read inverted.
//

#include "Expansion.h"
#include <stdio.h>

uint32_t expansionFastRamSize;

static enum { BOARD_NONE, BOARD_WAITING, BOARD_DONE } state;

// er_Type: Zorro II, linked into the memory list, the size code.
static uint8_t type_byte(void) {
    uint8_t code = 0;   // 000: 8 MB
    for (uint32_t size = 64u << 10; size < (8u << 20); size <<= 1)
        if (size == expansionFastRamSize)
            break;
        else
            code++;
    // 64 KB is code 1 ... 4 MB is code 7, 8 MB is code 0.
    code = (uint8_t)((code + 1) & 7);
    return 0xC0 | 0x20 | code;
}

// The board's register bytes, at offsets 0x00, 0x04, ... (before inversion).
static uint8_t register_byte(unsigned reg) {
    switch (reg) {
    case 0x00: return type_byte();
    case 0x04: return 0x01;     // er_Product
    case 0x08: return 0x80;     // er_Flags: prefer the 8 MB memory space
    case 0x10: return 0x12;     // er_Manufacturer (unregistered, "Omega")
    case 0x14: return 0x12;
    default:   return 0x00;     // serial number, diag vector: none
    }
}

void expansionReset(void) {
    const int was_mapped = state == BOARD_DONE;
    state = expansionFastRamSize ? BOARD_WAITING : BOARD_NONE;
    if (was_mapped)
        expansionHostMapFastRam(0, 0);
}

int expansionReadByte(uint32_t address) {
    if (state != BOARD_WAITING || (address & 1))
        return -1;
    const unsigned offset = address & 0xFFu;
    const uint8_t reg = register_byte(offset & ~3u);
    uint8_t nibble = (offset & 2) ? (reg & 0x0F) : (reg >> 4);
    if (offset >= 0x04 && offset < 0x40)
        nibble = ~nibble & 0x0F;
    return (nibble << 4) | 0x0F;
}

void expansionWriteByte(uint32_t address, uint8_t value) {
    if (state != BOARD_WAITING)
        return;
    switch (address & 0xFFu) {
    case 0x48: {    // er_BaseAddress, A23-A16: the board is configured
        const uint32_t base = (uint32_t)value << 16;
        state = BOARD_DONE;
        printf("Expansion: %lu KB Fast RAM at 0x%06lx\n",
               (unsigned long)(expansionFastRamSize >> 10),
               (unsigned long)base);
        expansionHostMapFastRam(base, expansionFastRamSize);
        break;
    }
    case 0x4C:      // shut up: the board stays away until the next reset
        state = BOARD_NONE;
        break;
    }
}
