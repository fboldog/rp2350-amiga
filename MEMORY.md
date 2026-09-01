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
| `omega/DMA.c` | `+#include <stdlib.h>` | `rand()` in `drawBlank()`; upstream got it transitively |
| `omega/DMA.c` | sprite2chunky ptr cast `uint8_t*`→`uint32_t*` (line ~697) | GCC 14 rejects incompatible pointer type; same byte address |
| `omega/Floppy.h` | `mfmData` is `uint8_t*` on PICO_BUILD | 2 MB inline array would be in SRAM; pointer into PSRAM instead |
| `omega/Floppy.c` | `ADF2MFM_from_mem()` added; original in `#ifndef PICO_BUILD` | No file I/O on RP2350; ADF lives in flash, passed as pointer |

---

## Known unknowns / risks

| Risk | Mitigation |
|------|-----------|
| PSRAM timing marginal at 250 MHz | Drop to 200 MHz if unstable; or adjust dummy cycles in psram.c |
| Musashi stack overflow | Add `pico_set_binary_type(omega-amiga no_flash)` or increase stack limit |
| KS 2.x/3.x don't reach a GUI | Omega display/chipset gap (upstream), not RP2350 or a CPU issue. Per Omega's README: KS 1.x boots fully (insert-disk screen + WB); KS 2.x/3.x "bootstrap" and can open an Intuition screen with working mouse/keyboard but don't render the insert-disk screen. A500/A600 KS 3.1 (40.63) and 3.2 are plain 68000 + OCS/ECS — no 68020 needed; only AGA-line ROM images (40.68, A1200/A4000) would also need an '020 core + AGA chipset. Incremental work, not a rewrite. |
| DMA cycle accuracy | Omega's DMA is approximate; will affect some demos, not WB |

---

## Completed sessions

### 2026-08-31 – Session 1
- Cloned Omega to `/tmp/omega-src`, analysed all source files
- Built Phase 1 port: all files in `/home/fboldog/source/repos/rp2350-amiga/`
- Created `combine_uf2.py` for ROM+firmware flashing
- Created `AGENT.md`, `TODO.md`, this `MEMORY.md`
- Next session: implement display output (Phase 2)

### 2026-09-01 – Session 2 (different machine: repo at `/root/source/repos/rp2350-amiga`)
- Set up toolchain: Arch Linux ARM (aarch64 host), pacman has no `arm-none-eabi-*`.
  Installed `cmake`+`ninja` via pacman; downloaded ARM GNU Toolchain 14.2.rel1
  (`aarch64-arm-none-eabi`) to `/root/toolchains/`. Pico SDK 2.1.1 cloned to
  `/root/pico-sdk` (with submodules).
- **Phase 1 code had never been compiled** – fixed 4 build breakers:
  1. `src/psram.c`: RP2350 has no `xip_ctrl_hw->flush` register – replaced the
     flush loop with `xip_cache_invalidate_all()` (`hardware/xip_cache.h`);
     added `hardware_xip_cache` to `target_link_libraries`.
  2. `src/main.c`: added `#include "hardware/clocks.h"` (for `set_sys_clock_khz`,
     `clock_get_hz`, `clk_sys`).
  3. `omega/DMA.c:697`: `sprite2chunky()` was passed a `uint8_t*` but the proto
     takes `uint32_t*` – GCC 14 makes `-Wincompatible-pointer-types` an error.
     Changed to `&((uint32_t*)host.pixels)[(mod+(Ny*640*4))/4]` (same address).
     Same bug is present in upstream Omega; older compilers only warned.
  4. `omega/DMA.c`: added `#include <stdlib.h>` for `rand()` in `drawBlank()`.
- **Clean build passes**: `build/omega-amiga.uf2` (515 KB). `arm-none-eabi-size`:
  text 257 KB (flash), bss 463 KB (of 520 KB SRAM – tight, mostly Musashi tables).
  Remaining warnings are all benign (upstream `m68kdasm.c` format-overflow,
  `CPU.c` `%x`/`uint32_t`, `debug.c` nested-comment, SDK/mbedtls sha512).
- Build command (env not persisted – export each session):
  ```
  export PICO_SDK_PATH=/root/pico-sdk
  export PATH=/root/toolchains/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi/bin:$PATH
  cd build && cmake .. -G Ninja -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release && ninja
  ```
- Not yet done: no hardware to flash/test; display output (Phase 2) still open.

**Native head-less runner added (`native/`)** to validate the core without hardware:
- Builds the same `omega/*.c` with `PICO_BUILD` undefined (desktop paths) +
  a `malloc` framebuffer + upstream `Memory.c` (`low16Meg`). No SDL; dumps PPM.
- `rp2350-emu`/`picoem` crate was evaluated and rejected: it does **not** emulate
  QMI or external PSRAM at `0x11000000`, and UART is a stub — our firmware faults
  on the first PSRAM access. It's a CPU/PIO validator, not a system emulator.
- Result: embedded Kickstart 1.3 boots to the insert-disk screen; `original2.adf`
  (WB 1.3.2 UK) boots through startup-sequence to `[CLI 2]`. So Musashi + Chipset
  + CIA + DMA + Blitter + Floppy all work. `src/psram.c` / `src/Memory.c` /
  `src/main.c` / PSRAM framebuffer are NOT covered (RP2350-only).
- Known cosmetic bug (both targets, same Blitter/DMA): the ROM diskette-logo
  bitmap on the insert screen renders horizontally mirrored; normal WB text is
  fine. Battery-clock at `0xDC0000` is a stub (`date` prints `<invalid>`).
- Build/run: `./native/build.sh` then
  `./native/omega-native <adf> 300000 50000 3000`; `native/ppm2png.py` for PNGs.

**Board retargeted to Waveshare RP2350-PiZero (2026-09-01).** User's test board.
- `src/board_config.h` — central pin/feature map. `src/boards/waveshare_rp2350_pizero.h`
  — Pico SDK board header (SDK 2.1.1 has none; `// pico_cmake_set PICO_PLATFORM=rp2350`).
  `CMakeLists.txt` now `PICO_BOARD=waveshare_rp2350_pizero` + `PICO_BOARD_HEADER_DIRS`.
  Builds as `rp2350-arm-s`. `psram.h`/`Memory.h`/`main.c` now pull from board_config.h.
- **RP2350B**, 16 MB flash. **PSRAM is NOT fitted from the factory** — empty 8-pin
  QSPI pad (U1) on the shared QMI bus, CS = **GPIO47**. User must solder an
  APS6404L-class part; 8 MB max on one die (their "16 MB" isn't possible on this
  board's single CS). `board_config.h` has `BOARD_HAS_PSRAM` (default 1).
- `src/psram.c` now does `gpio_set_function(47, GPIO_FUNC_XIP_CS1)` — the old code
  never routed CS1 (worked on Pico Plus 2 only because its SDK header did it).
- DVI/HDMI on-board: TMDS D0=GPIO36 D1=GPIO34 D2=GPIO32 CLK=GPIO38,
  invert_diffpairs=false, PIO0, gpio_base=16 (= PicoDVI `pico_sock_cfg`).
  DDC SDA=44/SCL=45, CEC=46.
- microSD: SPI1 SCK=30 MOSI=31 MISO=40 CS=43 (SDIO alt: D0=40 D1=41 D2=42 D3=43 CLK=38).
- WS2812 LED=GPIO2, I2C0 SDA=6/SCL=7, UART0 TX=0/RX=1. PIO-USB D+ pin is a guess
  (GPIO12) — verify against schematic before use.
- Pin data cross-checked: Waveshare RP2350-PiZero.pdf schematic + demo pack
  (01-DVI common_dvi_pin_configs.h, 03-MicroSD hw_config.c). Local copies were in
  /tmp/pizero-demo (gone next session — re-download from
  files.waveshare.com/wiki/RP2350-PiZero/RP2350-PiZero.zip / .pdf).
- Next session: implement display output (Phase 2) — PicoDVI via `BOARD_DVI_SERIALISER_CFG`.
