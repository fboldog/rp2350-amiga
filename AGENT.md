# Agent Instructions – Omega RP2350 Port

This is a bare-metal Amiga emulator port from Linux/macOS (SDL2) to the
RP2350B microcontroller. The only target is the **WeAct Studio RP2350B Core**
(populated PSRAM on GPIO0, HDMI on the HSTX pins GPIO12..19). All board pins
are in `src/board_config.h`; HDMI, SD and flash floppy are excluded by default
with CMake feature flags. Only DF0 is supported.

Read this file before doing any work. It tells you the architecture, what changed,
and where every important decision lives so you don't re-derive it.

---

## Project layout

```
rp2350-amiga/
├── CMakeLists.txt          Pico SDK 2.x; WeAct board; feature flags
├── pico_sdk_import.cmake   Existing-SDK lookup + pinned automatic fetch fallback
├── src/                    RP2350-specific code (overrides omega/ platform layer)
│   ├── board_config.h      ◀ ALL board pins / feature flags / PSRAM+Amiga map
│   ├── main.c              Bare-metal entry: PSRAM validation, ROM, emulation loop
│   ├── psram.h / psram.c   Simple hardware_psram availability + read/write test
│   ├── Memory.h / Memory.c PSRAM-backed chipRead*/chipWrite* (no 16 MB array)
│   ├── Host.h / Host.c     SDL-free host: raster presentation and frame submission
│   └── dvi_display.*      HSTX HDMI scanout: SRAM RGB332 frames, core-1 DMA IRQ
├── native/                 Head-less PC runner (see native/README.md)
├── omega/                  Upstream Omega source (minimal diffs from original)
│   ├── CPU.c               +#ifdef PICO_BUILD guard in cpu_pulse_reset
│   ├── Chipset.c           +chipramW = CHIPRAM_BASE_PTR (not low16Meg)
│   ├── DMA.c               +CHIPRAM_BASE_PTR for disk DMA; SDL_Atomic commented;
│   │                       +#include <stdlib.h>; sprite2chunky ptr cast (GCC14)
│   ├── Floppy.h            mfmData is a pointer; only DF0 has one
│   ├── Floppy.c            +ADF2MFM_from_mem(); DF0 only (DF1-3 unconnected)
│   └── [all other files]   UNCHANGED from upstream
└── tools/
    └── combine_uf2.py      Merges firmware.uf2 + ROM.bin + optional DF0 ADF
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
cmake .. -G Ninja   # PICO_BOARD defaults to weact_studio_rp2350b_core
ninja
# Output: build/omega-amiga.uf2
```

The default values of `OMEGA_ENABLE_HDMI`, `OMEGA_ENABLE_SDCARD` and
`OMEGA_ENABLE_FLASH_FLOPPY` are `OFF`; disabled subsystems are omitted from the
source and link lists. Pico SDK `hardware_psram` performs QMI setup before
`main()`. HDMI builds switch `clk_sys` to `OMEGA_SYS_CLK_KHZ` (default
320 MHz), retune `PLL_USB` to the TMDS bit rate for HSTX, move `clk_peri` to
`PLL_SYS`, and immediately retime PSRAM with the public SDK API.

If `PICO_SDK_PATH` is unset, configure fetches Pico SDK 2.3.1 into the build
tree. An explicit SDK checkout can still be selected with
`-DPICO_SDK_PATH=/path/to/pico-sdk`.

The verified WeAct HDMI build command is:

```bash
cmake -S . -B build-weact-hdmi -G Ninja -DOMEGA_ENABLE_HDMI=ON \
  -DOMEGA_ENABLE_FLASH_FLOPPY=ON -DOMEGA_DF0_INSERT_AT_BOOT=ON
cmake --build build-weact-hdmi -j
```

GCC 14 note: `-Wincompatible-pointer-types` is now an **error**, not a warning.
The Phase 1 tree had four such build breakers (now fixed) — see MEMORY.md
Session 2 if similar issues resurface after an upstream merge.

Flash with Kickstart ROM:
```bash
python3 tools/combine_uf2.py build/omega-amiga.uf2 kickstart13.rom combined.uf2
# Optional DF0 ADF:
python3 tools/combine_uf2.py build/omega-amiga.uf2 kickstart13.rom combined.uf2 \
    --adf0 workbench.adf
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
HDMI runs on the RP2350 HSTX peripheral (GPIO12..19), which TMDS-encodes
RGB332 in hardware. Two ping-pong DMA channels feed it a scanline at a time;
core 1 only handles one DMA interrupt per line (`hstx_dma_irq` in
`src/dvi_display.c`) and swaps frames at line 0. The emulator renders most
screens straight to RGB332 into double-buffered 640×240 (NTSC) or 640×256
(PAL) frames in SRAM (`hostDirect*` in `src/Host.c`); wrapped-fetch and HAM
screens still use the ARGB raster and presentation path. Verified on hardware
through the Kickstart boot screens, Workbench 1.3 and xsysinfo.

The interrupt must re-arm the finished channel before the other finishes its
line (~30 µs). Keep it in RAM and free of PSRAM/flash accesses: the emulator
saturates the shared QMI bus. HSTX only starts a few seconds after reset.

### Input
Wire `pressKey(keyCode)` / `releaseKey(keyCode)` to TinyUSB HID keyboard events.
The `keyMapping[]` table in `src/Host.c` already translates HID keycodes to
Amiga raw codes. Mouse delta → `chipset.joy0dat`, buttons → `CIAA.pra`.

### Dual-core
The emulator runs on core 0. Core 1 only services the HSTX DMA interrupt and
is otherwise idle (`__wfi`), so it can take emulator work later; shared-state
ownership must be designed first. Core 0 waits for a FIFO acknowledgement that
HSTX scanout started before it begins sustained PSRAM traffic.

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

- Debug output uses UART1 at 115200 (TX GPIO4, RX GPIO5). USB CDC is disabled.
- If PSRAM validation fails, first verify the CS pin (GPIO0) and that
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
- `*.adf` – Amiga Disk Format image for DF0 (optional)
- Pico SDK 2.x – fetched automatically at 2.3.1 unless `PICO_SDK_PATH` is set
