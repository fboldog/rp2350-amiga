//
//  Picasso2.h
//  Omega
//
//  A Village Tronic Picasso II+ RTG card: a Cirrus Logic GD5428 on Zorro II
//  with 2 MB of VRAM (board 2167/11) and 64 KB of registers (2167/12),
//  as WinUAE emulates it. Stock drivers drive it: PrismRTG's PICASSO2
//  driver and Picasso96's PicassoII card. Register semantics follow
//  PrismRTG's docs/cirrus-picasso2-hw.md.
//
//  The host owns the VRAM, maps the two boards where autoconfig puts them
//  (Expansion.c), routes CPU accesses here and shows the card's picture
//  (p2Showing, p2RenderLine) instead of the Amiga's while the monitor
//  switch selects it.
//

#ifndef Picasso2_h
#define Picasso2_h

#include <stdint.h>

#define P2_VRAM_SIZE 0x200000u
#define P2_REGS_SIZE 0x10000u

// The VRAM (P2_VRAM_SIZE bytes, 68k byte n = VRAM byte n) and power-on
// state. Call once.
void p2Init(uint8_t *vram);
// The Amiga's reset line: the monitor shows the Amiga again, the blitter
// stops.
void p2Reset(void);

// A byte access to the register board, at offset 0-0xFFFF.
uint8_t p2RegRead(uint32_t offset);
void p2RegWrite(uint32_t offset, uint8_t value);

// Nonzero while a blit takes its source from the CPU: every CPU write to
// the VRAM aperture is then source data (p2BltFeed), in 68k memory order,
// and must not reach VRAM.
extern volatile uint8_t p2CpuBlt;
void p2BltFeed(uint8_t value);

// The card's picture is on the monitor (switch on, screen on).
int p2Showing(void);
// The displayed mode: visible pixels.
void p2Geometry(int *width, int *height);
// One displayed line as 0x00RRGGBB pixels (width from p2Geometry), the
// hardware cursor included.
void p2RenderLine(int y, uint32_t *out);

#endif /* Picasso2_h */
