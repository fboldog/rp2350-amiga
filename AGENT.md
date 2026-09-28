# Agent Instructions – Omega RP2350 Port

This is a bare-metal Amiga emulator port from Linux/macOS (SDL2) to the
RP2350B microcontroller. Supported boards are the **Waveshare RP2350-PiZero**
(optional PSRAM on GPIO47) and **WeAct Studio RP2350B Core** (populated PSRAM
on GPIO0). All board pins are in `src/board_config.h`; HDMI and SD are
temporarily excluded by default with CMake feature flags.

Read this file before doing any work. It tells you the architecture, what changed,
and where every important decision lives so you don't re-derive it.

---

## Project layout

```
rp2350-amiga/
├── CMakeLists.txt          Pico SDK 2.x; Waveshare/WeAct board selection; feature flags
├── pico_sdk_import.cmake   Existing-SDK lookup + pinned automatic fetch fallback
├── src/                    RP2350-specific code (overrides omega/ platform layer)
│   ├── board_config.h      ◀ ALL board pins / feature flags / PSRAM+Amiga map
│   ├── boards/waveshare_rp2350_pizero.h   Pico SDK board header (local)
│   ├── main.c              Bare-metal entry: PSRAM validation, ROM, emulation loop
│   ├── psram.h / psram.c   Simple hardware_psram availability + read/write test
│   ├── Memory.h / Memory.c PSRAM-backed chipRead*/chipWrite* (no 16 MB array)
│   ├── Host.h / Host.c     SDL-free host: raster presentation and frame submission
│   └── dvi_display.*      PicoDVI core-1 scanout (WeAct: SRAM frames, full width)
├── native/                 Head-less PC runner (see native/README.md)
├── omega/                  Upstream Omega source (minimal diffs from original)
│   ├── CPU.c               +#ifdef PICO_BUILD guard in cpu_pulse_reset
│   ├── Chipset.c           +chipramW = CHIPRAM_BASE_PTR (not low16Meg)
│   ├── DMA.c               +CHIPRAM_BASE_PTR for disk DMA; SDL_Atomic commented;
│   │                       +#include <stdlib.h>; sprite2chunky ptr cast (GCC14)
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

The RP2350 port uses 8 MB PSRAM (memory-mapped at 0x11000000) with this layout
(defined in `src/board_config.h`, `BOARD_MAP_*`):

| PSRAM offset      | Content              | Amiga address     |
|-------------------|----------------------|-------------------|
| 0x000000–0x1FFFFF | Chip RAM (2 MB)      | 0x000000–0x1FFFFF |
| 0x200000–0x27FFFF | Slow/Ranger RAM      | 0xC00000–0xC7FFFF |
| 0x280000–0x47FFFF | DF0 MFM buffer (2MB) | (floppy drive 0)  |
| 0x480000–0x4FFFFF | SD ROM cache (512KB) | (optional)        |
| 0x500000–0x533FFF | DVI RGB332 buffers   | (Waveshare HDMI)  |
| 0x540000–0x63FFFF | Raw video raster     | (host rendering)  |
| 0x680000–0x77FFFF | Framebuffer 640×400  | (host output)     |

ROM is read-only in flash at 0x10200000 (absolute). `src/Memory.c:memory_init()`
validates it by checking `rom_base[0] == 0x11`.

The `CHIPRAM_BASE_PTR` macro in `src/Memory.h` expands to the PSRAM chip RAM base.
It is used by `omega/Chipset.c` (chipramW) and `omega/DMA.c` (disk DMA writes).

---

## Build

```bash
mkdir build && cd build
cmake .. -G Ninja   # PICO_BOARD defaults to waveshare_rp2350_pizero
ninja
# Output: build/omega-amiga.uf2
```

For WeAct use `-DPICO_BOARD=weact_studio_rp2350b_core`. The default values of
`OMEGA_ENABLE_HDMI` and `OMEGA_ENABLE_SDCARD` are `OFF`; disabled subsystems
are omitted from the source and link lists. Pico SDK `hardware_psram` performs
QMI setup before `main()`. HDMI builds switch to 252 MHz NTSC or 270 MHz PAL
and immediately retime PSRAM with the public SDK API.

If `PICO_SDK_PATH` is unset, configure fetches Pico SDK 2.3.1 into the build
tree. An explicit SDK checkout can still be selected with
`-DPICO_SDK_PATH=/path/to/pico-sdk`.

The verified WeAct HDMI build command is:

```bash
cmake -S . -B build-weact-hdmi -G Ninja \
  -DPICO_BOARD=weact_studio_rp2350b_core -DOMEGA_ENABLE_HDMI=ON
cmake --build build-weact-hdmi -j
```

GCC 14 note: `-Wincompatible-pointer-types` is now an **error**, not a warning.
The Phase 1 tree had four such build breakers (now fixed) — see MEMORY.md
Session 2 if similar issues resurface after an upstream merge.

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

### Display output
`display_push_frame()` presents the emulated raster into the 640×400 ARGB32
framebuffer, then queues it for PicoDVI (full-width DDF modes skip that pass
and convert the raw raster directly). Core 1 encodes double-buffered RGB332
frames to TMDS. The Waveshare RP2350-PiZero has HDMI on-board and uses
pixel-doubled 320×200 frames in PSRAM. WeAct has HDMI on GPIO12..19 and scans
out 640×200 at full width (no upscaling, so HIRES text keeps every pixel) with
the RP2350 SIO TMDS encoder; its frames are in internal SRAM. Verified on
hardware through the Kickstart boot screens and Workbench 1.3.

Solid red DVI lines mean core 1 missed a scanline deadline. Keep core 1's
per-line path free of PSRAM and flash accesses (no libc `memcpy`, no `const`
tables in `.rodata`): the emulator saturates the shared QMI bus.

### Input
Wire `pressKey(keyCode)` / `releaseKey(keyCode)` to TinyUSB HID keyboard events.
The `keyMapping[]` table in `src/Host.c` already translates HID keycodes to
Amiga raw codes. Mouse delta → `chipset.joy0dat`, buttons → `CIAA.pra`.

### Dual-core
The emulator remains on core 0 while PicoDVI owns core 1. Do not move the CPU
to core 1 without first redesigning the DVI worker and shared-state ownership.
Core 0 waits for a FIFO acknowledgement that `dvi_start()` completed before it
begins sustained PSRAM traffic; this is required for reliable cold startup.

---

## Running the core without hardware

`native/` builds the same `omega/*.c` for the host (no `PICO_BUILD`, no SDL) and
dumps framebuffer PPMs. Good for regression-checking CPU/chipset/DMA/Floppy after
an upstream merge:

```bash
./native/build.sh
./native/omega-native <workbench13.adf> 300000 50000 3000   # boots to [CLI 2]
python3 native/ppm2png.py frame_final.ppm frame_final.png
```

It does NOT cover `src/psram.c`, `src/Memory.c`, `src/main.c` or the PSRAM
framebuffer. The `rp2350-emu` / `picoem` crate is not usable here — it has no
QMI/PSRAM emulation, so the firmware faults on the first `0x11000000` access.

## Debugging tips

- Debug output uses UART1 at 115200 on WeAct (TX GPIO4, RX GPIO5) and UART0 at
  115200 on Waveshare (TX GPIO0, RX GPIO1). USB CDC is disabled.
- If PSRAM validation fails, first verify the selected board/CS pin and that
  `hardware_psram` reports the expected 8 MB; `src/psram.c` does not program QMI.
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
- Pico SDK 2.x – fetched automatically at 2.3.1 unless `PICO_SDK_PATH` is set
