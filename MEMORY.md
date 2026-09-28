# Project Memory – Omega RP2350 Port

Snapshot of all decisions, rationale, and context needed to resume work
on a fresh machine or in a new development session.

---

## Goal

Bare-metal Amiga emulator on **RP2350B** (Waveshare RP2350-PiZero and WeAct
Studio RP2350B Core, Cortex-M33).
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

### 5. Why do HDMI builds use 252/270 MHz?
PicoDVI serializes one TMDS bit per system-clock cycle, requiring 252 MHz for
720×480p60 NTSC or 270 MHz for 720×576p50 PAL. Pico SDK `hardware_psram`
configures QMI before `main()`; after changing `clk_sys`, the firmware calls
`psram_configure_params()` and `psram_reinitialize()` before accessing PSRAM.
Non-HDMI builds retain the SDK's default 150 MHz clock.

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

### 8. Emulator core on core 0, DVI on core 1
DMA and CPU remain interleaved on core 0 (200 DMA+CPU pairs per main-loop
iteration). PicoDVI TMDS encoding and PIO queueing run continuously on core 1.
Core 0 waits for a FIFO acknowledgement after core 1 completes `dvi_start()`;
without this handshake, emulator PSRAM traffic can race DVI initialization and
produce no signal on cold startup.

---

## PSRAM init notes

Pico SDK `hardware_psram` detects and configures QMI before `main()`. CMake sets
the 8 MB linker region and board-specific CS pin (GPIO0 WeAct, GPIO47 Waveshare).
`src/psram.c` contains no QMI register programming; it checks availability and
size, then verifies a 1024-byte deterministic pattern in a linker-placed PSRAM
buffer. This mirrors `/home/fboldog/source/repos/rp2350b-psram`.

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
| HDMI clock changes QMI timing | Reconfigure and reinitialize PSRAM through the SDK immediately after selecting 252/270 MHz |
| Musashi stack overflow | Add `pico_set_binary_type(omega-amiga no_flash)` or increase stack limit |
| KS 2.x/3.x don't reach a GUI | Omega display/chipset gap (upstream), not RP2350 / not a CPU issue. Tested KS 3.1 (exec 40.10) in the native runner 2026-09-01: (1) FIXED a hard crash — `sprite2chunky()` had no lower-bound check and KS 3.1 parks sprite 0 at X=-254 → OOB write (segfault native / silent PSRAM corruption on device); fixed in `src/Host.c` + `native/host_native.c` + guarded negative `Ny` in `omega/DMA.c`. (2) KS 3.1 now shows its grey WB screen but not the insert-disk graphic — `omega/Blitter.c` lacks "single pixel per H-line" (line draw) and "exclusive fill" modes that graphics.library 40.x uses. Implement those next. A500/A600 KS 3.1 (40.63)/3.2 are plain 68000 + OCS/ECS; only AGA ROM dumps (40.68) also need an '020 core + AGA chipset. |
| DMA cycle accuracy | Omega's DMA is approximate; will affect some demos, not WB |

---

## Completed sessions

### 2026-09-27 – WeAct PSRAM and DVI hardware validation
- Added automatic Pico SDK 2.3.1 fetching, WeAct board support, and independent
  CMake flags for HDMI, SD-card reading, and flash-backed floppy images.
- Replaced the custom QMI setup with Pico SDK `hardware_psram`; verified the
  populated WeAct 8 MB PSRAM through startup testing and full emulator use.
- Wired an Adafruit DVI breakout to GPIO11..18 and UART1 to GPIO4/5. D0 had to
  be reversed physically for correct colors. USB CDC remains disabled.
- Added core-1 PicoDVI output, PSRAM RGB332 double buffering, uncached scanout,
  and a core-1 startup acknowledgement that prevents a cold-boot DVI race.
- Corrected the 400-row render path and LORES overscan origin. The complete,
  centered Kickstart 1.3 hand/floppy animation is stable on captured hardware.
- Flash Kickstart loading is verified. The bring-up image intentionally omits
  ADFs; flash floppy and SD support remain compile-time disabled by default.
- PAL and NTSC native regression suites pass with the intentional insert-screen
  baseline updates. The verified combined UF2 contains firmware plus a legally
  supplied Kickstart ROM only.

### 2026-08-31 – Session 1
- Cloned Omega to `/tmp/omega-src`, analysed all source files
- Built Phase 1 port: all files in `/home/fboldog/source/repos/rp2350-amiga/`
- Created `combine_uf2.py` for ROM+firmware flashing
- Created `AGENT.md`, `TODO.md`, this `MEMORY.md`
- At that point, the next task was display output; it was completed and
  hardware-validated in the 2026-09-27 session above.

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
- At that point no hardware had been flashed; see the 2026-09-27 validation
  session above for the current result.

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

**KS 3.1 investigation (2026-09-01, native runner).** User supplied a KS 3.1
ROM (exec 40.10). Native runner gained `OMEGA_ROM=<file>` / `OMEGA_DISASM=1`.
- Found + fixed a real OOB bug: `sprite2chunky()` only clamped the upper edge;
  KS 3.1 parks sprite 0 at X=-254 → `pixBuff[-254]` write → SIGSEGV (native) /
  silent wild write ~1 KB before the PSRAM framebuffer (device). Fixed in
  `src/Host.c` + `native/host_native.c`; also guard negative sprite row `Ny`
  in `omega/DMA.c`. Upstream Omega has the same bug.
- After the fix KS 3.1 comes up to its grey Workbench screen but the
  insert-disk graphic doesn't draw. `omega/Blitter.c` stubs two modes KS 3.1
  uses: **one-dot-per-h-line** line draw (`bltcon1` bit 1 SING, printf at
  ~L178) and **area fill** inclusive/exclusive (`bltcon1` bits 3/4, printfs at
  ~L434/438). Full implementation guide is in **TODO.md** under the KS 3.1
  item ("Implementing the two missing blitter modes"). This is the handoff
  point — continuing on the other machine.
- Next for KS 3.1 remains implementing those two blitter modes. PicoDVI output
  was completed later and is no longer an open item.

**WeAct full-width HDMI (2026-09-28).** Goal: no upscaling on WeAct HDMI
(GPIO12..19, Pico DVI Sock ordering).
- The old 360-wide doubled path sampled every second raster column, dropping
  half of each HIRES pixel: Workbench 1.3 fonts looked broken. WeAct now uses
  720-wide RGB332 lines, 640 image columns 1:1.
- PicoDVI has no full-res 8bpp encoder. `tmds_encode_palette_data` is an
  interpolator/LUT path and far too slow on core 1 (solid red screen). The
  fix drives the RP2350 SIO TMDS encoder directly with pixel doubling off
  (`dvi_encode_channel_fullres_8bpp`, `tmds_encode_sio_loop_poppop_ratio2`).
- Red DVI lines = PicoDVI "late scanline". Measured over SWD: encode <=20 us,
  but 720-byte PSRAM line copies took up to 116 us under emulator QMI load.
  PSRAM already runs at its 133 MHz limit. Fix: both 640x200 frames in SRAM
  (250 KB), made possible by a 16-bit Musashi opcode index (Pico builds) plus a
  flash-resident `const` opcode descriptor table (~150 KB SRAM freed). WeAct
  uses 4 TMDS buffers (heap is tight: ~17 KB spare after them).
- Core 1 must not touch flash either: libc `memcpy` (runs from flash) and a
  `static const` blank line (in `.rodata`) each caused red lines.
- Handy debugging: `openocd ... -c init -c "echo [capture {mdw ADDR N}]"` reads
  RAM without halting; `dump_image` of the SRAM frames renders what core 1 is
  scanning out.
- Gotcha: `build-weact-hdmi/` had been copied from the `rp2350-amiga-codex`
  checkout and built that tree's sources. Check `CMAKE_HOME_DIRECTORY` in
  CMakeCache.txt when builds "do nothing".
- The pending Floppy change moves the `/CHNG` acknowledge from drive select to
  head step and reports `/DKRDY` on insert/eject, so runtime insertion is seen
  by a waiting Kickstart.
- Capture-chain gotcha: after reflashing, the HDMI capture device can lose
  lock and stay black even though the board is fine. Confirm scanout over SWD
  (DVI `timing_state` counter advancing, `dump_image` of `sram_frames`), then
  reset the capture device.

**Workbench 1.3 top line (2026-09-28).** The extra-word HIRES layout (DDF
0x3c-0xd0) anchored raster row 0 at DIWSTRT line 5 + 40 = line 45, but
Intuition's screen begins at line 44 (0x2c), so the title bar's top padding
line was dropped on every target. `OMEGA_DDF_EXTRA_UPPER_OVERSCAN` is now 39;
only the `wb13` regression baselines (NTSC and PAL) changed, after review.

**NTSC DVI mode (2026-09-28).** NTSC builds ran PicoDVI's 720x480p60 timing
(needs 270 MHz) at the 252 MHz NTSC clock, emitting a non-standard ~55.9 Hz
mode; the capture device locked onto it unreliably. NTSC now uses VGA
640x480p60 (exactly 252 MHz), which also fits the 640-wide image without side
borders. Raising NTSC to 270 MHz was rejected: PSRAM would drop from 126 MHz
(div 2) to 90 MHz (div 3). `dvi_display_init()` panics on any clk_sys/timing
mismatch.

**PAL boot screens / UF2 (2026-09-28).**
- PAL lost capture lock during Kickstart's solid grey/white boot screens and
  recovered at the AmigaDOS window. Not signal integrity (8 mA fast pads did
  nothing) and not timing (`late_scanline_ctr` stayed 0). It was the content:
  bright full-width image with black 40-pixel side columns. Borders now show
  COLOR00 (per-frame `frame_border`, repainted by core 1 on change).
- `combine_uf2.py` numbered appended ROM/ADF blocks separately from the
  firmware in the same family; the RP2350 boot ROM stops after the first
  block's numBlocks, so a new ADF was never written via BOOTSEL. Now one
  sequence per family; other families (E10 workaround block) untouched.
