//
//  Expansion.h
//  Omega
//
//  Zorro II autoconfig at 0xE80000 with one board: Fast RAM. Kickstart's
//  expansion code reads the board's description, gives it a base address
//  (0x200000 for the first memory board) and adds the memory to the free
//  list. The host keeps the memory and maps it (expansionHostMapFastRam).
//

#ifndef Expansion_h
#define Expansion_h

#include <stdint.h>

#define EXPANSION_BASE 0xE80000u
#define EXPANSION_END  0xE90000u

// The board's size: a power of two from 64 KB to 8 MB, or 0 for no board.
// The host sets it before the first reset.
extern uint32_t expansionFastRamSize;

// The reset line: the board is unconfigured again and its memory unmapped
// (the contents stay, as on real hardware).
void expansionReset(void);
// A byte read in 0xE80000-0xE8FFFF: the register, or -1 when no board
// answers (none, configured, or shut up).
int expansionReadByte(uint32_t address);
// A byte write there (er_BaseAddress at 0x48 configures, 0x4C shuts up).
void expansionWriteByte(uint32_t address, uint8_t value);

// Host side: map size bytes of Fast RAM at base, or unmap (size 0).
void expansionHostMapFastRam(uint32_t base, uint32_t size);

#endif /* Expansion_h */
