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
- [x] **KS 3.1 blitter modes implemented (2026-09-01).** Tested with KS 3.14 ROM.
      - [x] FIXED: hard crash in `sprite2chunky()` — no lower-bound check; KS 3.1
            parks sprite 0 at X = -254 → `pixBuff[-254]` OOB write.
      - [x] IMPLEMENTED: SING (one-dot-per-h-line, `bltcon1` bit 1) — all 8 octants
            in `blitter_execute()`. `lastRow` guard tracks the current raster row
            (= `i` for y-dominant cases 0–3, = `d` for x-dominant cases 4–7).
      - [x] IMPLEMENTED: IFE/EFE area fill (`bltcon1` bits 3/4). `carry` resets to
            FCI at each row start; propagates LSB→MSB across each `channelD` word;
            applied after `logicFunction()`, before channel-D store.
      - Result: KS 3.14 with no disk boots ROM-based Workbench (grey backdrop +
        title bar rendered). KS 3.1 boots from its internal ROM disk when no floppy
        is inserted — no "insert disk" requester fires in this scenario. The title
        bar text IS rendered via blitter line+fill (visible in frame captures).
        Full WB rendering verification requires a WB3.1 ADF.
      - Note: A500/A600 KS 3.1 (40.63) / 3.2 are plain 68000 + OCS/ECS — no
        68020 needed. Only AGA-line ROM dumps (40.68) would also need an '020
        core + AGA chipset.

- [x] **KS 2.0.4 floppy boot fixed (2026-09-02).** Now boots Workbench 2.x from ADF.
      Three root causes identified and fixed:
      1. `omega/CIA.c` `CIAInit`: `CIAA.pra` init `0x37` → `0xF7`. Bits 6 (/FIR0)
         and 7 (/FIR1) are joystick fire-button inputs (active-low); they must start
         at 1 (not pressed). KS 2.04 polls bit6 at 0xFC91AA and loops forever if 0.
      2. `omega/CIA.c` `CIAWrite` case 0: `value & 63` → `(pra & 0xC0) | (value & 0x3F)`.
         Bits 6-7 are input-only pins; every PRA write was zeroing them, defeating fix 1.
      3. `omega/CIA.c` ICR/IRQ logic: the interrupt-request bit (ICR bit 7) must only be
         set when the individual event bit is enabled in `icrMask`. The old code ORed
         the mask directly into `icr` which garbled both the status and the IR flag.
      4. `omega/Floppy.c` idMode: empty drive slots (`hasDisk=0`) now return `/DKRDY=1`
         during the 32-pulse ID probe → ID=0xFFFFFFFF (absent). Previously all drives
         returned `/DKRDY=0` → ID=0x00000000 (present), causing Workbench to show
         spurious `DF1:????` `DF2:????` `DF3:????` icons for unconnected drives.
      5. `omega/Floppy.c` `/CHNG` acknowledge: asserting SEL (drive select low) now sets
         `df[n].pra |= 0x04` to clear the disk-change latch, matching real hardware.
      6. `omega/Floppy.c` `floppyInit`: initialises `hasDisk=0`, `/CHNG=1` (stable),
         `/DKRDY=1` (not ready) for all four drive slots; drive 0's `hasDisk` is set to
         1 and `/CHNG` asserted only when an ADF is successfully loaded.
      Result: KS 2.04 + `Install3.2.adf` boots to the Workbench 2.x desktop in ~1000
      VBLs. Title bar, Ram Disk, and the Install3.2 volume all render correctly.

- [ ] **Slow RAM shadow**: Omega's original code has `#define NOSLOWRAM` to mirror
      chipset registers at 0xC00000. This is not implemented in `src/Memory.c`.
      Add if KS 1.x boot hangs.
- [ ] **Stack size**: Musashi uses recursion for instruction dispatch. Default Pico
      stack (2 KB) may be too small. Add `pico_set_binary_type(omega-amiga copy_to_ram)`
      or increase stack in linker script if crashes occur.
- [ ] **Blitter accuracy**: Omega blitter is "immediate mode" (fires all at once).
      Works for most software but some demos rely on cycle-accurate blitter timing.
      Low priority.
