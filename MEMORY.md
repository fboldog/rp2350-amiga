# Project Memory – Omega RP2350 Port

Snapshot of all decisions, rationale, and context needed to resume work
on a fresh machine or in a new Claude session.

---

## Goal

Bare-metal Amiga emulator on **RP2350B** (Pimoroni Pico Plus 2, Cortex-M33).
Source: [Omega](https://github.com/h5n1xp/Omega) – a clean-room, dependency-free
Amiga emulator designed explicitly for bare-metal porting.

Target constraints:
- 520 KB internal SRAM (too small for Amiga RAM)
- 8 MB PSRAM (APS6404L, QMI CS1) – used for all Amiga RAM
- 16 MB Flash (QMI CS0) – firmware + ROM + floppy images
- No OS, no libc heap, no SDL, no file I/O

---

## Architecture decisions and their reasons

### 1. Why not use the 16 MB `low16Meg` array?
The original Omega declares `unsigned char low16Meg[16777216]` as a global.
At 16 MB, this doesn't fit in 520 KB SRAM. The RP2350 has 8 MB PSRAM but
it's external (QMI interface), not SRAM. Solution: replace `low16Meg` with
address-translated PSRAM access in `src/Memory.c`. The Amiga only actually
uses ~3 MB (2 MB chip + 512 KB slow + 512 KB ROM), so the 8 MB PSRAM is sufficient.

### 2. Why is the include path ordered `src/` before `omega/`?
Omega files `#include "Memory.h"` and `#include "Host.h"` (capital). The RP2350
replacements live in `src/Memory.h` and `src/Host.h`. Putting `src/` first in
`target_include_directories` causes the RP2350 versions to shadow the originals
without modifying the omega source files.
**Critical**: do NOT add `omega/Memory.h` or `omega/Host.h` to the project.

### 3. Why only 2 floppy drives?
Each drive's MFM buffer is `12798 × 82 × 2 = 2,098,872 bytes ≈ 2 MB`.
Four drives = 8 MB, which would use the entire PSRAM before chip RAM even gets allocated.
Two drives = 4 MB, leaving 4 MB for chip RAM, slow RAM, framebuffer, and scratch.

### 4. Why is ROM in flash (not PSRAM)?
Flash (CS0) is 16 MB and read-only via XIP at 0x10000000. Placing ROM at flash
offset 0x200000 (absolute 0x10200000) means zero PSRAM consumed and zero startup
copy needed. The Kickstart ROM is only ever read (never written). ROM reads in
`src/Memory.c` just return `rom_ptr(address)` which is a direct flash pointer.

### 5. Why overclock to 250 MHz?
The original Omega on RPi3 ran at 1.2 GHz. Musashi 68K executing ~1 M68K instruction
per Amiga system cycle needs all the speed available. At 150 MHz (stock) the emulator
is noticeably slow. 250 MHz is stable with the VREG at 1.15 V on RP2350. PSRAM QSPI
runs at 125 MHz (clkdiv=2) which is within the APS6404L spec.

### 6. Why `#ifdef PICO_BUILD` instead of separate source files?
Omega is designed so that only the "Host layer" needs porting. The few exceptions
(cpu_pulse_reset, chipramW assignment, disk DMA byte writes) are tiny, isolated
changes. Using `#ifdef` guards keeps the diff minimal and lets the desktop build
still compile for testing on Linux/macOS.

### 7. Why is `chipramW` a word-indexed pointer?
`internal.chipramW` is `uint16_t*` pointing to chip RAM base. The bitplane pointers
(`bpl1pt` etc.) are stored as **word addresses** (byte_address >> 1) by the write
handlers in Chipset.c (e.g. `chipset.bpl1pt = (value << 15) | ...`). So
`chipramW[bpl1pt]` correctly addresses `byte_offset = bpl1pt * 2`. On PSRAM,
`chipramW = (uint16_t*)(PSRAM_BASE + PSRAM_CHIPRAM_OFFSET)` is correct.

### 8. Single-threaded for now (Phase 1)
Omega's original SDL version had a commented-out CPU thread. Phase 1 runs
DMA + CPU interleaved on core 0 (200 DMA+CPU pairs per main loop iteration,
same as the original SDL main loop). Phase 2 will split: DMA on core 0 timer
ISR, CPU on core 1.

---

## PSRAM init notes

`src/psram.c` manually drives QMI CS1 to:
1. Send Reset Enable (0x66) + Reset (0x99) in direct/SPI mode
2. Send Enter Quad Mode (0x35)
3. Configure QMI CS1 timing, read format (0xEB quad read, 6 dummy cycles), write format

The PSRAM then appears at 0x11000000 and is accessed like regular memory.
If the init fails (wrong clkdiv or dummy cycles for your clock rate), the symptom
is garbled reads from PSRAM. Verify by writing a known pattern and reading it back
immediately after `psram_init()` in `src/main.c`.

---

## Omega source changes summary (what was modified and why)

| File | Change | Reason |
|------|--------|--------|
| `omega/CPU.c` | `#ifdef PICO_BUILD` in `cpu_pulse_reset` | `low16Meg` not available; call `memory_clear_chipram()` instead |
| `omega/Chipset.c` | `chipramW = CHIPRAM_BASE_PTR` | Points into PSRAM instead of `low16Meg` |
| `omega/DMA.c` | `CHIPRAM_BASE_PTR` for disk DMA writes | Same reason |
| `omega/DMA.c` | SDL_Atomic calls commented out | No threading in Phase 1; no SDL |
| `omega/Floppy.h` | `mfmData` is `uint8_t*` on PICO_BUILD | 2 MB inline array would be in SRAM; pointer into PSRAM instead |
| `omega/Floppy.c` | `ADF2MFM_from_mem()` added; original in `#ifndef PICO_BUILD` | No file I/O on RP2350; ADF lives in flash, passed as pointer |

---

## Known unknowns / risks

| Risk | Mitigation |
|------|-----------|
| PSRAM timing marginal at 250 MHz | Drop to 200 MHz if unstable; or adjust dummy cycles in psram.c |
| Musashi stack overflow | Add `pico_set_binary_type(omega-amiga no_flash)` or increase stack limit |
| KS 3.x ROMs don't boot | Omega limitation, not RP2350; KS 1.3 and 2.x work |
| DMA cycle accuracy | Omega's DMA is approximate; will affect some demos, not WB |

---

## Completed sessions

### 2026-08-31 – Session 1
- Cloned Omega to `/tmp/omega-src`, analysed all source files
- Built Phase 1 port: all files in `/home/fboldog/source/repos/rp2350-amiga/`
- Created `combine_uf2.py` for ROM+firmware flashing
- Created `AGENT.md`, `TODO.md`, this `MEMORY.md`
- Next session: implement display output (Phase 2)
