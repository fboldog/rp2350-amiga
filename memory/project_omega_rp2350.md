---
name: omega-rp2350-port
description: Omega Amiga emulator RP2350 bare-metal port – architecture, status, and key design decisions
metadata:
  type: project
---

Port of Omega (https://github.com/h5n1xp/Omega) to RP2350B (Pimoroni Pico Plus 2) in Cortex-M33 mode.

Project directory: /home/fboldog/source/repos/rp2350-amiga/

## Phase 1 complete (2026-08-31):
- Build system (Pico SDK 2.x, pico2 board)
- PSRAM init (APS6404L on QMI CS1 → 0x11000000)
- Memory.c replaced with PSRAM-backed version
- Host.c replaced (SDL removed, framebuffer in PSRAM)
- Floppy: ADF2MFM_from_mem() added for memory-buffer encoding
- All SDL_Atomic refs guarded with #ifndef PICO_BUILD

## Key design decisions:

**Memory layout (8MB PSRAM at 0x11000000):**
- 0x000000–0x1FFFFF: Chip RAM (2MB)
- 0x200000–0x27FFFF: Slow RAM (512KB)
- 0x280000–0x47FFFF: DF0 MFM buffer (2MB)
- 0x480000–0x67FFFF: DF1 MFM buffer (2MB)
- 0x680000–0x77FFFF: Framebuffer 640×400 ARGB32 (1MB)

**ROM in flash:** Kickstart ROM placed at flash offset 0x200000 (absolute 0x10200000).
tools/combine_uf2.py merges firmware.uf2 + kickstart.rom + optional ADFs.

**Only 2 floppy drives supported** (insufficient PSRAM for 4).

## Phase 2 TODOs:
- Display output: implement display_push_frame() in src/Host.c
  (PicoDVI / VGA-PIO / SPI TFT)
- USB HID: wire pressKey/releaseKey to TinyUSB HID events
- Dual-core: put CPU on core 1 (skeleton in src/main.c)

**Why:** User wants bare-metal Omega-style emulation on RP2350B as a personal project.
**How to apply:** All changes are in /home/fboldog/source/repos/rp2350-amiga/.
