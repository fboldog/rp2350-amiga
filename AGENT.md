# Agent Instructions – Omega RP2350 Port

This is a bare-metal Amiga emulator port from Linux/macOS (SDL2) to the
RP2350B microcontroller (Pimoroni Pico Plus 2, Cortex-M33, 8 MB PSRAM).

Read this file before doing any work. It tells you the architecture, what changed,
and where every important decision lives so you don't re-derive it.

---

## Project layout

```
rp2350-amiga/
├── CMakeLists.txt          Pico SDK 2.x build; board=pico2; -DPICO_BUILD=1
├── pico_sdk_import.cmake   Standard SDK import helper (copy from SDK)
├── src/                    RP2350-specific code (overrides omega/ platform layer)
│   ├── main.c              Bare-metal entry: overclock, PSRAM, ROM, emulation loop
│   ├── psram.h / psram.c   QMI CS1 init for APS6404L 8 MB PSRAM at 0x11000000
│   ├── Memory.h / Memory.c PSRAM-backed chipRead*/chipWrite* (no 16 MB array)
│   └── Host.h / Host.c     SDL-free host: PSRAM framebuffer, UART printf, stubs
├── omega/                  Upstream Omega source (minimal diffs from original)
│   ├── CPU.c               +#ifdef PICO_BUILD guard in cpu_pulse_reset
│   ├── Chipset.c           +chipramW = CHIPRAM_BASE_PTR (not low16Meg)
│   ├── DMA.c               +CHIPRAM_BASE_PTR for disk DMA; SDL_Atomic commented
│   ├── Floppy.h            +mfmData is uint8_t* on PICO_BUILD (not inline array)
│   ├── Floppy.c            +ADF2MFM_from_mem(); original ADF2MFM in #ifndef guard
│   └── [all other files]   UNCHANGED from upstream
└── tools/
    └── combine_uf2.py      Merges firmware.uf2 + ROM.bin + optional ADFs
```

---

## How the include path works (CRITICAL)

`CMakeLists.txt` lists `src/` BEFORE `omega/` in `target_include_directories`.

This means every `#include "Memory.h"` in omega files resolves to `src/Memory.h`
(the PSRAM version), not the original Omega `Memory.h` (which is not in the repo).
Same for `Host.h`. Do NOT add `omega/Memory.h` or `omega/Host.h` to the project.

---

## Memory architecture

The original Omega used:
```c
unsigned char low16Meg[16777216];  // 16 MB static array – impossible on RP2350
```

The RP2350 port uses 8 MB PSRAM (memory-mapped at 0x11000000) with this layout:

| PSRAM offset      | Content              | Amiga address     |
|-------------------|----------------------|-------------------|
| 0x000000–0x1FFFFF | Chip RAM (2 MB)      | 0x000000–0x1FFFFF |
| 0x200000–0x27FFFF | Slow/Ranger RAM      | 0xC00000–0xC7FFFF |
| 0x280000–0x47FFFF | DF0 MFM buffer (2MB) | (floppy drive 0)  |
| 0x480000–0x67FFFF | DF1 MFM buffer (2MB) | (floppy drive 1)  |
| 0x680000–0x77FFFF | Framebuffer 640×400  | (host output)     |

ROM is read-only in flash at 0x10200000 (absolute). `src/Memory.c:memory_init()`
validates it by checking `rom_base[0] == 0x11`.

The `CHIPRAM_BASE_PTR` macro in `src/Memory.h` expands to the PSRAM chip RAM base.
It is used by `omega/Chipset.c` (chipramW) and `omega/DMA.c` (disk DMA writes).

---

## Build

```bash
export PICO_SDK_PATH=/path/to/pico-sdk   # SDK 2.x required
mkdir build && cd build
cmake .. -DPICO_BOARD=pico2
make -j$(nproc)
# Output: build/omega-amiga.uf2
```

Flash with Kickstart ROM:
```bash
python3 tools/combine_uf2.py build/omega-amiga.uf2 kickstart13.rom combined.uf2
# Optional ADFs:
python3 tools/combine_uf2.py build/omega-amiga.uf2 kickstart13.rom combined.uf2 \
    --adf0 workbench.adf --adf1 extras.adf
# Copy combined.uf2 to board in BOOTSEL mode
```

---

## The compile-time guard pattern

All RP2350-specific code in omega/ files uses:
```c
#ifdef PICO_BUILD
    // RP2350 path
#else
    // Desktop (SDL/Linux) path
#endif
```

`PICO_BUILD=1` is set in CMakeLists.txt via `target_compile_definitions`.

---

## What still needs doing (see TODO.md for full detail)

### Most impactful next step: display output
Implement `display_push_frame()` in `src/Host.c`. The framebuffer is already rendered
into PSRAM at `PSRAM_BASE + PSRAM_FRAMEBUF_OFFSET` in ARGB32, 640×400.

Three options (pick one):
1. **SPI TFT** (ILI9341/ST7789) – easiest hardware, lowest effort
2. **VGA via PIO** (pico-extras scanvideo) – good resolution, resistor DAC needed
3. **DVI/HDMI via PicoDVI** – best quality, needs HDMI breakout board

### Input
Wire `pressKey(keyCode)` / `releaseKey(keyCode)` to TinyUSB HID keyboard events.
The `keyMapping[]` table in `src/Host.c` already translates HID keycodes to
Amiga raw codes. Mouse delta → `chipset.joy0dat`, buttons → `CIAA.pra`.

### Dual-core
`src/main.c` has a commented-out `core1_entry()` stub. When display output is
working, move `cpu_execute()` to core 1 and run DMA on a hardware timer alarm
on core 0. Add a spinlock for shared chipset register access.

---

## Debugging tips

- UART0 (TX=GPIO0, RX=GPIO1) at 115200. `printf()` works via `pico_stdio_uart`.
- If PSRAM test fails at boot, check `src/psram.c` timing constants against the
  APS6404L datasheet at your clock frequency. Try reducing `clkdiv` or increasing
  dummy cycle count.
- If the emulator hangs immediately after ROM starts executing:
  - Verify ROM was placed at flash offset 0x200000 (check with picotool)
  - Check `rom_base[0]` in memory_init() via UART output
  - Enable the Musashi disassembler: set `disass = 1` in CPU.c; output goes to UART
- If malloc-related linker errors appear: Floppy.c still has `malloc` inside
  `#ifndef PICO_BUILD` – verify that guard is in place.

---

## Files NOT in this repo (user must supply)

- `kickstart13.rom` – Kickstart 1.3 (or 2.x) ROM binary (copyright Commodore/Cloanto)
- `*.adf` – Amiga Disk Format images for floppy drives (optional)
- Pico SDK 2.x – set `PICO_SDK_PATH` environment variable
