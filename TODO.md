# TODO – Omega RP2350 Port

## Legend
- [x] Done
- [ ] Not started
- [~] In progress / partial

---

## Phase 1 – Compile target (COMPLETE — clean build verified 2026-09-01)

Toolchain: ARM GNU Toolchain 14.2.rel1 (aarch64-arm-none-eabi) + Pico SDK 2.1.1.
Output `build/omega-amiga.uf2` builds clean. Not yet run on hardware.

Core logic verified via `native/` head-less runner (2026-09-01): Kickstart 1.3 +
`original2.adf` boot to the AmigaDOS CLI on a PC build of the same `omega/*.c`.
Covers Musashi/Chipset/CIA/DMA/Blitter/Floppy; NOT the RP2350 `src/` layer.
(The `rp2350-emu` crate can't run the firmware — no QMI/PSRAM emulation.)

- [x] Project structure: `omega/` (upstream), `src/` (RP2350 platform)
- [x] CMakeLists.txt for Pico SDK 2.x, board `pico2`
- [x] `src/psram.c` – QMI CS1 init for APS6404L at 0x11000000
- [x] `src/Memory.c` – PSRAM-backed chipRead*/chipWrite* (replaces 16 MB array)
- [x] `src/Host.c` – SDL-free host; framebuffer pointer in PSRAM; UART printf
- [x] `src/main.c` – bare-metal entry, overclock to 250 MHz, PSRAM/memory/CPU init
- [x] `omega/CPU.c` – `#ifdef PICO_BUILD` guard around `low16Meg` clear in `cpu_pulse_reset`
- [x] `omega/Chipset.c` – `chipramW` set via `CHIPRAM_BASE_PTR` macro
- [x] `omega/DMA.c` – disk DMA writes use `CHIPRAM_BASE_PTR`; SDL_Atomic calls commented
- [x] `omega/Floppy.h` – `mfmData` is `uint8_t*` (pointer) on PICO_BUILD
- [x] `omega/Floppy.c` – `ADF2MFM_from_mem()` added (no malloc/lseek/read)
- [x] `tools/combine_uf2.py` – merges firmware.uf2 + ROM + optional ADFs
- [x] Build fixes (never compiled before 2026-09-01):
      `src/psram.c` XIP flush → `xip_cache_invalidate_all()` (RP2350 has no
      `xip_ctrl_hw->flush`); `src/main.c` +`hardware/clocks.h`; `omega/DMA.c`
      +`<stdlib.h>` and sprite2chunky `uint8_t*`→`uint32_t*` cast (GCC 14)

---

## Phase 2 – Display output

Target board is the **Waveshare RP2350-PiZero** (see `src/board_config.h`),
which has an on-board **DVI/HDMI** connector — so Option C below is the natural
choice. Pin map is already in `board_config.h`:
TMDS D0=GPIO36, D1=GPIO34, D2=GPIO32, CLK=GPIO38, `invert_diffpairs=false`,
PIO0, `pio_set_gpio_base(pio0, 16)` — identical to PicoDVI's `pico_sock_cfg`.
Use `BOARD_DVI_SERIALISER_CFG` to init libdvi.

Pick ONE output method and implement `display_push_frame()` in `src/Host.c`.

### Option A: SPI TFT (easiest hardware, lowest bandwidth)
- [ ] Choose driver chip: ILI9341 (240×320) or ST7789 (240×240 or 320×240)
- [ ] Add SPI TFT driver (bare-metal or from Pimoroni C++ SDK)
- [ ] In `display_push_frame()`: scale/crop 640×400 → 320×240 and push via SPI DMA
- [ ] Configure SPI GPIO pins in CMakeLists.txt (add `hardware_spi`)

### Option B: VGA via PIO (best resolution, needs resistor DAC)
- [ ] Wire 3-bit R, 3-bit G, 2-bit B resistor ladder + HSync + VSync (5 GPIOs total)
- [ ] Port or import pico-vga scanvideo library (in pico-extras)
- [ ] Set up scanvideo for 640×480 @ 60 Hz
- [ ] In DMA `evenCycle` VBL handler, push each scanline to the VGA scanvideo buffer
- [ ] Add `pico_scanvideo_dpi` to target_link_libraries

### Option C: DVI/HDMI via PicoDVI  ◀ RP2350-PiZero has this on-board
- [ ] Add PicoDVI `libdvi` (Waveshare ship a known-good copy in their
      RP2350-PiZero demo pack; or upstream Wren6991/PicoDVI)
- [ ] `struct dvi_serialiser_cfg cfg = BOARD_DVI_SERIALISER_CFG;` +
      `pio_set_gpio_base(pio0, BOARD_DVI_GPIO_BASE);`
- [ ] Pick timing: 640×480p60. Reconcile `BOARD_SYS_CLK_KHZ` with the DVI bit
      clock (252 MHz full-res, ~126 MHz scanbuf). May need to drop the CPU
      overclock or run DVI on its own clock.
- [ ] Allocate DVI scanline/framebuffer in PSRAM; convert Omega's ARGB32
      640×400 → RGB565 (letterbox to 480 lines) in `display_push_frame()`
- [ ] DVI IRQ + scanout on core 1 (see Phase 4)

---

## Phase 2b – microSD (optional, RP2350-PiZero has a slot)

- [ ] Add a FatFS + SD-SPI driver (Waveshare bundle no-OS-FatFS; pins in
      `board_config.h`: SPI1 SCK30/MOSI31/MISO40/CS43)
- [ ] Load Kickstart ROM + ADF images from the card instead of baking them into
      flash with `combine_uf2.py` — makes disk swapping practical
- [ ] Optional: a tiny on-screen disk chooser

---

## Phase 3 – Input

- [ ] Enable TinyUSB host mode (`pico_stdlib` already pulls it in if board has USB)
- [ ] Implement USB HID keyboard callback → call `pressKey()` / `releaseKey()`
      - HID key codes map to SDL keycodes; the keyMapping[] table in Host.c handles the rest
- [ ] Implement USB HID mouse callback → update `chipset.joy0dat` and `CIAA.pra`
      - Mouse X/Y delta: `joy0dat = (dy << 8) | (dx & 0xFF)`
      - Left button: `CIAA.pra &= ~(1<<6)` (pressed) / `|= (1<<6)` (released)
      - Right button: `chipset.potinp &= ~(1<<10)` (pressed)
- [ ] Add `tinyusb_host`, `tinyusb_board` to target_link_libraries in CMakeLists.txt
- [ ] Optional: PS/2 keyboard via GPIO PIO (simpler than USB, no hub support)

---

## Phase 4 – Dual-core operation

Currently both DMA and CPU run on core 0 in a tight loop.
Splitting onto two cores removes the manual interleaving and makes both faster.

- [ ] Core 0: DMA engine (`dma_execute()` in a timer ISR at PAL pixel clock rate)
- [ ] Core 1: Musashi 68K CPU (`m68k_execute()` in a tight loop)
- [ ] Add spinlock or semaphore for shared chipset register access
      (chipset.intreqr, chipset.intena, copper PC writes are the hot paths)
- [ ] Uncomment `core1_entry` skeleton in `src/main.c`
- [ ] DMA timer: use `hardware_timer` alarm at ~7.09 MHz (PAL pixel clock / 1)
      or simply at line rate (15.625 kHz) and execute 227 DMA cycles per alarm

---

## Phase 5 – Audio

Omega has audio register stubs but no PCM output.

- [ ] Implement `audio0Cycle`–`audio3Cycle` in `omega/DMA.c` to generate 8-bit samples
- [ ] Set up I2S or PWM audio output on RP2350
      - PWM: `hardware_pwm` at 44.1 kHz, mix 4 channels → mono/stereo
      - I2S: use PIO + `hardware_pio`; needs I2S DAC (PCM5102 etc.)
- [ ] Ring-buffer between DMA audio cycles and audio output ISR

---

## Phase 6 – CPU / memory performance (do only if profiling shows a need)

Musashi at 250 MHz ≈ 10–12× an A500's 68000, so this is a "later" item. The
likely real costs on RP2350 are XIP-cache misses on the interpreter hot path
and QSPI round-trips to chip RAM in PSRAM — not the interpreter itself.
Decided against swapping Musashi for an Emu68-style JIT: Emu68 is an AArch64
JIT that needs ~30 host regs + an MMU + an RWX cache in fast RAM, none of which
exist on Cortex-M33 (see MEMORY.md). Measure before touching any of this.

- [ ] **Profile first.** Add a cycle counter (DWT CYCCNT / `systick`) around
      `cpu_execute()` vs `dma_execute()` vs `hostDisplay()` for a WB boot, and
      count `chipRead*/chipWrite*` calls by address range. Confirm it is
      CPU/memory-bound before optimizing.
- [ ] **Keep Musashi's tables in SRAM** (they already are by default —
      `m68ki_instruction_jump_table` 256 KB @ 0x2000_9b88, `m68ki_cycles`
      192 KB @ 0x2004_9e68). Guard against a future refactor pushing them to
      flash; the `m68ki_cpu` register struct (272 B) is already SRAM-resident.
- [ ] **Run the Musashi hot path from SRAM**, not XIP flash. Tag the dispatch
      loop + the most common opcode handlers (MOVE, ADD/SUB, Bcc, JSR/RTS,
      LEA, CMP, Tcc, ANDI/ORI to CCR) with `__not_in_flash_func(...)` so
      interpreter dispatch does not stall on XIP-cache misses. Watch SRAM
      budget — only ~40–50 KB headroom; move the hottest handlers only, by
      profile.
- [ ] **SRAM window over low chip RAM.** The 68k SSP/USP stacks, the exception
      vector table (0x0000–0x03FF) and hot Exec structures all cluster in low
      chip RAM. Back the first 64–128 KB of chip RAM with an SRAM array and
      keep the rest in PSRAM; add an `addr < BOARD_CHIPRAM_SRAM_WINDOW`
      branch to `chipRead*/chipWrite*` and `CHIPRAM_BASE_PTR` users
      (`omega/Chipset.c` chipramW, `omega/DMA.c` disk DMA). One compare+branch
      per access vs. a saved QSPI transaction on the hottest region.
- [ ] **Consider const Musashi tables in flash.** If SRAM is tight after the
      above, pre-generate `m68ki_instruction_jump_table` / `m68ki_cycles` as
      `const` (XIP-cached reads) to reclaim ~448 KB SRAM for the chip-RAM
      window. Requires baking the tables at build time instead of
      `m68k_build_opcode_table()` at boot.
- [ ] Re-profile after each step; stop when WB feels responsive.

---

## Known issues / investigation needed

- [ ] **PSRAM timing at 250 MHz**: `src/psram.c` uses `clkdiv=2` (QSPI at 125 MHz).
      Verify APS6404L spec allows 125 MHz at 3.3 V; may need dummy-cycle count tweak.
      Test by reading back a written pattern at startup.
- [ ] **`chipset.vposr` PAL/NTSC flag**: currently hardcoded to NTSC value `0x1000`.
      PAL should be `0x0000`. Change in `omega/DMA.c:dma_execute()`.
- [ ] **ROM validation**: `src/Memory.c:memory_init()` checks `rom_base[0] == 0x11`.
      KS 2.x ROMs start with `0x11 0x14`; KS 1.3 with `0x11 0x11`. KS 3.1/3.2
      also start `0x11 ..` so they pass and execute.
- [~] **KS 3.1 boots but doesn't render the insert-disk screen.** Tested
      2026-09-01 with a KS 3.1 ROM (exec 40.10) in the native runner.
      - [x] FIXED: hard crash in `sprite2chunky()` — no lower-bound check, and
            KS 3.1 parks sprite 0 (the pointer) at X = -254, so `pixBuff[-254]`
            = OOB write (segfault native / silent PSRAM corruption on RP2350).
            Also guarded a negative sprite row (`Ny < 0`) at `omega/DMA.c:699`.
            Upstream Omega has the same bug; it just didn't fault under SDL.
      - [ ] KS 3.1 now brings up its grey Workbench screen and starts drawing
            but the insert-disk graphic doesn't appear. `omega/Blitter.c` logs
            "No Single pixel per H-line mode yet" and "NO EXCLUSIVE FILL MODE
            YET!!" — graphics.library 40.x uses blitter one-dot line draw and
            exclusive area-fill, which Omega stubs with a printf. **Next step:
            implement both in `blitter_execute()` (see the guide below).**
      - Note: A500/A600 KS 3.1 (40.63) / 3.2 are plain 68000 + OCS/ECS — no
        68020 needed. Only AGA-line ROM dumps (40.68) would also need an '020
        core (`m68kconf.h` 010/020 `OPT_OFF`) + AGA chipset (`Chipset.c:713`).

      #### Implementing the two missing blitter modes (`omega/Blitter.c`)

      `blitter_execute(Chipset_t*)` is the immediate-mode blitter (the
      `OblitterExecute` / `blitterCycle` cycle-accurate skeleton is an unused
      empty stub). It splits on `bltcon1 & 1`: set = **line mode** (8 octants,
      `case 0..7`), clear = **area/copy mode** (channels A/B/C/D, minterms,
      masks, shifts, ascending/descending). Both paths currently ignore the
      two features KS 3.1 needs.

      BLTCON1 bit map (for reference):
      | bit | area mode | line mode |
      |----|-----------|-----------|
      | 0  | LINE=0    | LINE=1 |
      | 1  | DESC (address decrement) | SING (one dot per h-line) |
      | 2  | FCI (fill carry input)   | SUD  (octant select) |
      | 3  | IFE (inclusive fill)     | SUL  (octant select) |
      | 4  | EFE (exclusive fill)     | AUL  (octant select) |
      | 12-15 | BLTBSH (B/pattern shift) | texture start bit |

      **(a) One-dot-per-horizontal-line** — `bltcon1 bit 1` (SING) in line mode.
      `Blitter.c:175` already reads `oneDot`; line ~178 just prints. In the
      octant loops the Bresenham step does `d += 1` (a step along the minor
      axis) only when `D > 0`, otherwise it stays on the same minor-axis row.
      With SING set, plot the pixel **only on the iteration where the minor
      axis (the raster line) is about to change or on the first pixel of a new
      line** — i.e. suppress the `chipramW[addr] = pixel` write on steps where
      the current raster line equals the previously plotted one. Concretely:
      keep `int lastRow = INT_MIN;` before the loop; compute this step's row
      (for x-dominant octants 0/1/4/6 the row is the running minor coordinate
      `d` plus the plane's base row; for y-dominant octants 2/3/5/7 it is `i`);
      write the pixel only when `row != lastRow`, then `lastRow = row`. Used
      by graphics.library for un-fillable outlines and `Move()/Draw()` with a
      write mask that must not double-hit a scanline.

      **(b) Area fill (inclusive `IFE` / exclusive `EFE`)** — `bltcon1` bits
      3/4; `fillmode = (bltcon1 & 0x18) >> 3` (1 = inclusive, 2 = exclusive),
      already computed at `Blitter.c:426`. Fill always runs with **DESC set**
      (right-to-left), which is why the current stub sits inside
      `if(bltcon1 & 2)` — keep that. Also read `int fci = (bltcon1 >> 2) & 1;`
      (not currently read).

      Fill runs on channel **D after the logic function**, per row, LSB→MSB of
      each output word as the blit walks right-to-left, with a 1-bit `carry`
      that is reset to `fci` at the **start of every row** (`for y` loop) and
      carried across words within the row. Per output bit `b`:
      ```
      inclusive: out = b | carry;   carry ^= b;
      exclusive: out = b ^ carry;   carry ^= b;   // i.e. out = carry_after
      ```
      Apply this to `channelD` before the byte-swap + `chipramW[dpt] = ` store
      in the area-mode inner loop (around `Blitter.c:539-552`). Because the
      area loop already runs `x` from 0..sizeh-1 with `xIncrement == -1`, the
      words arrive in right-to-left order; iterate the 16 bits of each
      `channelD` from bit 0 upward. Reset `carry = fci` in the `for(y…)` body,
      before the `for(x…)` loop.

      Verify with the native runner:
      `OMEGA_ROM=kick31.rom ./native/omega-native "" 120000 15000 999999`
      then `native/ppm2png.py frame_final.ppm out.png` — success = the
      insert-disk graphic (hand + disk) appears instead of the bare grey
      screen. The "…YET!!" printfs should stop. Re-check KS 1.3 still boots
      to `[CLI 2]` and demos with fills (e.g. any WB 1.3 program that draws
      filled shapes) still look right.
- [ ] **Slow RAM shadow**: Omega's original code has `#define NOSLOWRAM` to mirror
      chipset registers at 0xC00000. This is not implemented in `src/Memory.c`.
      Add if KS 1.x boot hangs.
- [ ] **Stack size**: Musashi uses recursion for instruction dispatch. Default Pico
      stack (2 KB) may be too small. Add `pico_set_binary_type(omega-amiga copy_to_ram)`
      or increase stack in linker script if crashes occur.
- [ ] **Blitter accuracy**: Omega blitter is "immediate mode" (fires all at once).
      Works for most software but some demos rely on cycle-accurate blitter timing.
      Low priority.
