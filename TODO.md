# TODO – Omega RP2350 Port

## Legend
- [x] Done
- [ ] Not started
- [~] In progress / partial

---

## Phase 1 – Compile target (COMPLETE — emulator and HDMI verified on hardware)

Toolchain: ARM GNU Toolchain 14.2.rel1 (aarch64-arm-none-eabi) + Pico SDK 2.3.1.
Output `omega-amiga.uf2` targets the WeAct Studio RP2350B Core (the Waveshare
RP2350-PiZero was dropped on 2026-09-29: its HDMI is on GPIO32..39, out of
HSTX's reach). PSRAM, flash Kickstart loading, emulation, UART, and full-width HDMI
on GPIO12..19 are verified through the Kickstart 1.3 boot screens and
Workbench 1.3.

Core logic verified via `native/` head-less runner (2026-09-01): Kickstart 1.3 +
`original2.adf` boot to the AmigaDOS CLI on a PC build of the same `omega/*.c`.
Covers Musashi/Chipset/CIA/DMA/Blitter/Floppy; NOT the RP2350 `src/` layer.
(The `rp2350-emu` crate can't run the firmware — no QMI/PSRAM emulation.)

- [x] Project structure: `omega/` (upstream), `src/` (RP2350 platform)
- [x] CMakeLists.txt for Pico SDK 2.x and the WeAct RP2350B board
- [x] `src/psram.c` – simple SDK-backed availability/size/pattern validation,
      based on the known-good `rp2350b-psram` project
- [x] `src/Memory.c` – PSRAM-backed chipRead*/chipWrite* hardware-validated
- [x] `src/Host.c` – SDL-free host; framebuffer pointer in PSRAM; UART printf
- [x] `src/main.c` – bare-metal entry and PSRAM/memory/CPU init sequence;
      HDMI clock selection safely retimes PSRAM through the SDK
- [x] `omega/CPU.c` – `#ifdef PICO_BUILD` guard around `low16Meg` clear in `cpu_pulse_reset`
- [x] `omega/Chipset.c` – `chipramW` set via `CHIPRAM_BASE_PTR` macro
- [x] `omega/DMA.c` – disk DMA writes use `CHIPRAM_BASE_PTR`; SDL_Atomic calls commented
- [x] `omega/Floppy.h` – `mfmData` is a pointer; only DF0 has a buffer
- [x] `omega/Floppy.c` – `ADF2MFM_from_mem()` added (no malloc/lseek/read)
- [x] `tools/combine_uf2.py` – merges firmware.uf2 + ROM + optional DF0 ADF
- [x] Build fixes (never compiled before 2026-09-01):
      `src/psram.c` XIP flush → `xip_cache_invalidate_all()` (RP2350 has no
      `xip_ctrl_hw->flush`); `src/main.c` +`hardware/clocks.h`; `omega/DMA.c`
      +`<stdlib.h>` and sprite2chunky `uint8_t*`→`uint32_t*` cast (GCC 14)

---

## Phase 2 – Display output (DVI COMPLETE ON WEACT)

HDMI runs on the RP2350 HSTX peripheral (GPIO12..19), verified with Kickstart
1.3/2.04, Workbench 1.3 and xsysinfo in NTSC and PAL. VGA and SPI TFT remain
alternative future backends.

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

### Option C: DVI/HDMI via HSTX
- [x] First bring-up with PicoDVI (PIO + core-1 software TMDS), replaced by
      HSTX on 2026-09-29 and removed with the Waveshare board
- [x] Standard 640×480p60 (252 Mbit/s) for NTSC and 720×576p50 (270 Mbit/s)
      for PAL; `clk_hstx` = `PLL_USB / 2`, `clk_sys` chosen separately
- [x] 640×240 (NTSC) / 640×256 (PAL) frames in SRAM (colour numbers,
      expanded to RGB888 at scanout), COLOR00 borders via HSTX `TMDS_REPEAT`
      commands
- [x] Hardware TMDS encoding; ping-pong DMA with one core-1 interrupt per
      line, startup acknowledgement to core 0

---

## Phase 2b – microSD (optional, external SPI module)

- [x] Add a read-only FatFs + SD-SPI driver; default pins in
      `board_config.h` (SPI1 SCK30/MOSI31/MISO40/CS43), overridable with
      `-DBOARD_SD_*`
- [~] Load Kickstart ROM from `/rom/kick13.rom`, with flash fallback. The code
      compiles with `-DOMEGA_ENABLE_SDCARD=ON` but needs an SD module wired to
      the WeAct board for validation. The DF0 ADF remains flash-backed.
- [ ] Stream ADF tracks from SD instead of storing a complete MFM image in
      PSRAM; this is needed for practical disk swapping
- [ ] Optional: a tiny on-screen disk chooser

---

## Phase 3 – Input

- [x] **USB host (2026-10-03):** TinyUSB host with hub on the native USB-C
      port, HID boot protocol (`src/usb_input.c`). `clk_sys` 336 MHz (7 × 48
      MHz) gives the USB clock from `PLL_SYS`. Board power must go to the
      VBUS pin so the USB-C VBUS carries 5 V. Mouse (JOY0DAT, both buttons)
      and keyboard (arrow keys in RemGame) verified through a powered hub.
- [x] **USB keymap and keyboard reset (2026-10-03).** `keyMapping[]` (Host.c)
      is indexed by SDL key codes, not HID usages, so typing was wrong; USB
      keys now map by position to Amiga raw keycodes (`hid_to_amiga[]`) via
      `hostAmigaKey()`. Ctrl + both Amiga keys used the RESET-instruction
      handler from inside `dma_run()`, leaving the 68000 stopped (white
      screen); it now does a real 68000 + chip reset between slices. A CIA
      interrupt pending at the reset left `irqLatch` set (CIA-A never
      interrupted again, boot hung): `CIAInit()` clears latch, mask and
      control registers. Verified on all three disks.
- [x] **Board reset from the keyboard:** Ctrl + Alt + Delete reboots the
      RP2350 through the watchdog (next ADF in the rotation); Ctrl + both
      Amiga keys stays an Amiga-only reset. Verified both.
- [ ] Keyboard: report-protocol keyboards that ignore boot protocol.
- [ ] Joysticks / gamepads (port 2: JOY1DAT, CIA-A /FIR1).
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

Omega has audio register stubs but no PCM output. Plan (2026-10-03): HDMI
audio over the existing cable, no extra hardware; PWM/I2S stay as fallbacks.

- [x] **Paula emulation (2026-10-03)** in `omega/Audio.c`: DMA channels with
      block reload and audio interrupts, CPU-written AUDxDAT, periods,
      volume, advanced once per raster line. Fixed: audio DMA tested
      `chipset.dmacon` (never written), AUD1VOL wrote channel 0. RemGame's
      music verified in the native build (`OMEGA_PCM`). Cost ~3 % (RemGame
      31.0 → 30.0 fps), ~4 % idle xSysinfo; ~1 % is the audio work, the rest
      could not be pinned down (not flash placement, not loop alignment).
- [ ] Attach modes (ADKCON USE*V*/USE*P*): volume/period modulation.
- [x] **Mixer and output stage (2026-10-04):** 48 kHz stereo, box-filtered,
      A500 low-pass (~4.9 kHz) and LED filter (~3.3 kHz Butterworth).
- [x] **HDMI audio over HSTX (2026-10-04)**, `OMEGA_HDMI_AUDIO` (NTSC
      default ON): data islands per line (audio sample + ACR/InfoFrames),
      video preamble/guard, table-driven encoding in the DMA interrupt.
      Scanout became four DMA channels (blanking + active per line; two lost
      sync) with row expansion in a lower-priority software interrupt (a
      slow HAM row delayed re-arming by up to 338 µs). Slow emulation:
      tried holding the last sample (buzz), stretching by resampling (pitch
      drops), fading gaps (choppy); kept WSOLA time-stretching on core 1,
      bit-exact bypass within 2 % of real time. Verified by ear on a
      monitor (the capture card records HDMI audio as silence).
- [x] **Colour register writers in RAM (2026-10-04).** RemGame's Copper
      writes colours every line; the 32 `colorNN()` writers (157 bytes each,
      in flash) now share one `setColour()` in RAM (0.4 KB): RemGame 28.7 →
      29.0 fps. `Audio.c` in RAM as well gave only +0.5 % for 2.6 KB of SRAM
      (NTSC would keep 0.7 KB), so it stays in flash.
- [x] **HDMI audio on PAL (2026-10-04)**: default ON; the PAL core-1 ring
      shrinks to ~45 KB and the stretcher's output ring moved to SCRATCH_X.
      Along the way: 4-sample packets every 2-3 lines with blanking buffers
      reused (only packet 1 rewritten), WSOLA matched on a decimated mono
      copy (worst step 170k -> 71k cycles), pre-encoded silent packets after
      a second of silence (stretcher stopped), and no bus priority for
      core 1 (it slowed core 0 ~2 %: PAL xSysinfo 50/36 -> 50/49; NTSC
      RemGame 30.0 -> 31.4, PAL no audio 25.4 -> 26.9). A queue of
      thread-encoded packets (0.67 ms) underran in RemGame (audible gaps)
      and was dropped.
- [ ] **Amiga Test Kit display** (`image_refs/atk-pal.png`, `atk-ntsc.png`):
      map beam lines to fixed image rows per standard instead of the
      per-layout anchors (ATK's full-width hires window at line 0x46 loses
      its top 14 lines and sits at row 0), and record COLOR00 per line in
      the borders (ATK's white lines and blue/red bands are Copper colour
      changes outside the window; the pointer is missing too). Recheck
      Workbench, the Kickstart screens, RemGame and D-Mob against their
      references.
- [ ] **PAL lines are 228 colour clocks, should be 227** (`SLOT_LAST`
      0xE3, Audio.c `SLOTS_PER_LINE`): ATK times CIA E-clock ticks over 5
      frames (71,364 here, threshold 71,264, real PAL 71,051) and reports
      OCS/NTSC; PAL frame rate and Paula pitch are 0.4 % off. NTSC should
      alternate 227/228 (LOL).
- [ ] Unexplained, seen once each during PAL audio benchmarks: a stall
      booting Workbench (not reproduced), and a core-1 fault record (IRQ 51,
      a user IRQ, jumped to 0; possibly from a wedged chip / rescue cycle).
- [ ] Fallbacks: PWM on a spare GPIO (RC filter), or an I2S DAC (PCM5102)
      via PIO.

---

## Phase 6 – Performance (profiled 2026-09-28)

**Baseline:** WeAct, PAL build, idle Workbench 1.3: CIA-A TOD advanced 39 ticks
in 10 s, i.e. ~3.9 emulated vblanks/s vs 50 on an A500 (~8 % of real speed,
~900 clk_sys cycles per DMA slot + CPU slice). A PAL Workbench boot takes ~4 min.

**Profile** (4000 core-0 PC samples via `DWT_PCSR`, same state):

| Share | Function | Work |
|---|---|---|
| 34.6 % | `hostClearRaster` | clear the 1 MB ARGB32 raster in PSRAM every frame |
| 23.6 % | `hiresPlanar2Chunky` | write ARGB32 pixels into that PSRAM raster |
| 11.2 % | `dvi_display_submit_raster` | read it back, convert to RGB332 |
| 10.5 % | `dma_execute` | per-slot chipset dispatch |
| ~7 % | `hiresPlane1`, `plane2..5` | bitplane fetch slots |
| 3.0 % | `m68k_execute` | the 68000 itself |
| ~3 % | `eclock_execute`, `CIAExecute` | CIA timers every slot |
| 1.6 % | `copperExecute` | Copper |

~70 % of core 0 is the display pipeline's PSRAM traffic; the CPU is not the
bottleneck yet. Re-profile after each step: the ranking will change.

Neither Omega nor Musashi has a JIT; Musashi is a table-dispatched
interpreter. An Emu68-style JIT was ruled out: Emu68 targets AArch64 and needs
an MMU, many host registers and a large RWX translation cache, none of which a
Cortex-M33 with ~11–80 KB of free SRAM provides. Prefer SRAM-resident hot
handlers and cheaper memory paths (P2).

How to measure (no halt, firmware keeps running):
- Speed: read `CIAA.tod` (address via `arm-none-eabi-gdb -batch -ex "p/x
  (int)&CIAA.tod" <elf>`) twice over SWD, 10 s apart.
- Profile: `mww 0xE000EDFC` with TRCENA (bit 24) set, then read
  `mdw 0xE000101C` (`DWT_PCSR`) a few thousand times in one OpenOCD session and
  bucket the samples with `arm-none-eabi-nm -n` / `addr2line`.
- [ ] Add both as a script (`tools/profile_pc.py`) so every change is measured.
- [ ] Also profile a Workbench *boot* (disk-heavy) separately from idle.

### P1 – Display pipeline (~70 %)
- [x] **Stop clearing the 1 MB PSRAM raster every frame** (was 34.6 %). The
      DMA write sites report a written `[min, max)` column range per raster row
      (`hostRasterWritten()`); the HDMI raster path reads only those pixels and
      shows COLOR00 elsewhere, so no clear is needed. The presentation
      fallback border-fills unwritten pixels once after such frames.
      Result (PAL, WeAct): idle Workbench 3.9 → 5.9 vblanks/s (≈1.5×); boot to
      the Workbench icons ~250 s → 151.5 s. New top costs:
      `hiresPlanar2Chunky` 35.2 %, `dvi_display_submit_raster` 19.7 %,
      `dma_execute` 14.6 %, `m68k_execute` 4.4 %.
- [x] **Render straight to RGB332.** For full-width fetch layouts on WeAct,
      the DMA write sites (`hostRasterPixels()` / `hostRasterWritten()`) hand
      each 16/32-pixel ARGB block to the host, which converts it to RGB332
      directly into the free SRAM scanout frame, applying the raster-row
      mapping (LORES offset, alternate rows, DDF rotation) captured at frame
      start. Unwritten image pixels get COLOR00 at vblank. The PSRAM raster is
      untouched; narrow layouts, Waveshare and native keep the ARGB paths.
      Result (PAL, WeAct): boot to the Workbench icons 151.5 s → 93.5 s;
      just after reaching Workbench 5.9 → 11.1 vblanks/s.
      Verified on NTSC and PAL hardware.
- [x] **Table-driven planar → RGB332.** `hostDirectHires`/`hostDirectLores`
      convert 16-pixel blocks with a 2 KB byte-spread table (8 colour indices
      per OR of the plane entries) and `internal.palette332` (kept in sync with
      every `internal.palette` write), skipping the per-bit loop and the ARGB
      step. HAM, narrow layouts, Waveshare and native keep the ARGB path.
      Verified identical to the bit loops on 400,000 random blocks. Results:
      NTSC KEY insert → AmigaDOS 78.8 s → 64.3 s; PAL boot to the Workbench
      icons 86.8 s → 82.1 s; idle Workbench 14.2 vblanks/s.
- [x] **Narrow fetch layouts on the direct path.** DDFSTRT >= 0x40 screens
      (e.g. the KS 2.04 insert screen) used the old presentation path (1 MB
      framebuffer fill, raster copy and clear, 400-row readback) and ran at
      ~2.1 vblanks/s, so the disk animation looked frozen. They now render
      straight to RGB332 with hostPresentFrame()'s mapping (row offset
      (HOST_CONTENT_Y - viewport) / 2, column - HOST_FETCH_LEAD clipped to
      [HOST_VISIBLE_X0, HOST_VISIBLE_X1)): ~5 vblanks/s, animation visible.
      Layouts needing the wrapped-fetch reconstruction keep the old path.
- [x] **NTSC shows 240 rows** (the full 640x480 mode) instead of 200, so
      overscan screens such as xsysinfo are no longer cut off.
- [x] **Exact colours.** RGB332 could not represent many Amiga colours
      (KS 2.04 disk shutter $998877 showed olive; blue had 4 levels; white
      was 224). Frames now hold colour numbers or raw HAM codes; core 1 logs
      palette changes and the pixel runs drawn after them, and the scanout
      interrupt replays the log and expands each row once to RGB888 (exact
      12-bit colours, mid-line Copper changes kept, HAM decoded per row from
      COLOR00). HSTX sends RGB888. Verified: Workbench 1.3, the KS 2.04
      insert screen (122 Copper colour changes), RemGame (HAM). Speed
      unchanged: KS1.3+WB1.3 PAL 44.6 s, 29.4/20.6 emulated/shown; NTSC
      45.7 s, 35.0/27.0. HAM decoding first ran over the ~32 µs line
      deadline (60 % of core 1, lost HDMI sync); fixed by re-arming the DMA
      channel before expanding and a branchless, pair-wise HAM decode (18 %).
      PAL went back to a 16 KB ring for the frame records (~470 B SRAM left).
- [x] **DF0 disk rotation (demo mode).** Up to 15 ADFs back to back from
      flash 0x280000 (`combine_uf2.py --adf ... --adf ...`, end marker after
      the last); each boot, reset included, mounts the next one. Position in
      PSRAM 0x7FF000 (kept through RUN-pin resets, lost on power loss; no
      flash writes). POWMAN scratch registers were cleared by the RESET
      button (RUN pin), though they survive SWD resets.
      Verified: SWD resets rotate 1 → 2 → 3 → 1; the RESET button advances
      the disk; a power cycle starts at the first.
- [x] **Two-level opcode table; 64 KB rings.** `m68ki_instruction_index`
      (128 KB flat) became a byte per 64-opcode block plus the ~245 unique
      blocks (~33 KB); built flat in PSRAM scratch at boot and verified.
      ~96 KB SRAM back for <1 % idle speed (boot +1-2 s). A 64 KB ring then
      shows every emulated frame on idle Workbench (PAL 28.0/28.0, was
      28.2/21.7; NTSC 33.6/33.6, was 33.6/32.5); 32 KB did not help PAL and
      112/128 KB add nothing. SRAM left: PAL 57 KB, NTSC 77 KB — not enough
      for a third frame (PAL 160 KB, NTSC 150 KB), even with smaller rings.
- [x] **Frame records in PSRAM; rings take the rest of SRAM.** The two
      `dvi_indexed_frame_t` records moved to PSRAM (0x780000, cached alias,
      coherent through the shared XIP cache): ~9.6 KB SRAM back for ~1 %
      speed, and the limits grew to 1024 runs / 1024 palette changes
      (RemGame filled the 448-entry log with sprite colours). The ring no
      longer needs a power-of-two size: PAL 16 → 24 KB, NTSC 32 → 44 KB.
      Idle Workbench emulated/shown: PAL 29.1/20.5 → 28.4/21.6 (boot 45.7 →
      47.6 s), NTSC 35.0/27.0 → 33.8/32.3 (45.7 → 46.6 s). ~2 KB SRAM left.
- [x] ~~**PSRAM framebuffer.**~~ Measured and dropped (2026-09-29). Reading
      a frame from PSRAM is free with one continuous XIP stream per frame
      started at vertical blanking (no underruns even under the KS 2.04
      animation); per-row DMA reads cost 18 % emulation and the XIP stream
      restarted per row missed 27 % of row deadlines under load. Writing
      the frames is the problem: core-1 writes cost 14 % emulation, DMA
      copies (paced or not) also stall the HSTX DMA (RGB888: the 8-word FIFO
      holds ~0.3 µs) and dropped display frames (51-59/s).
- [ ] **Smaller frames.** Colour numbers need at most 6 bits; packing 16-colour
      screens at 4 bits per pixel would free ~80 KB per frame (HAM/EHB rows
      still need a byte) — room for a larger PAL ring or a third frame.
- [ ] **Exact colours for wrapped-fetch layouts** (still ARGB → RGB332).
- [x] **Sprites.** Sprite DMA (`spriteCycle()`: POS/CTL after vertical
      blanking, DATA/DATB from VSTART, the next POS/CTL at VSTOP), DATA arms
      and CTL disarms (CPU or DMA), clipped to the display window, attached
      pairs (each half at its own position), BPLCON2 playfield priority
      (single playfield), HAM-safe overlay on core 1 (0x40 pixel tag). 32-bit
      CPU writes to handler-less register pairs (SPRxPT, BPLxPT) are now two
      16-bit writes on the board. Palette log 2047 entries. Workbench shows
      the pointer; RemGame shows its HUD, player and items. Cost: Workbench
      idle 28.0 -> 27.2 frames/s, RemGame 10.5 -> 9.6 (PAL).
- [ ] **Sprite collisions (CLXDAT/CLXCON)** and **dual playfield** (full
      BPLCON2 priorities) are not emulated.
- [x] **RemGame item colours: CPU:beam ratio.** The emulated 68000 got 16
      cycles per DMA slot (Omega's value) where an A500 gets 2 (7.09 MHz CPU,
      3.55 MHz colour clock), so it ran ~8x fast against the beam, and the
      Musashi overrun was dropped on top. RemGame races the beam (it polls
      VPOSR/VHPOSR ~240k times/s, never CLXDAT) to refill each 16-line band's
      sprites and colours; too fast, it gave the heart a flask's colours and
      lost items. `OMEGA_CPU_CYCLES_PER_SLOT` (default 2) with the overrun
      carried: 2 and 4 match the reference (image_refs/remgame-*.png), 6 and
      8 do not. PAL: RemGame 9.0 -> ~17-18 fps (busy-waits no longer emulated
      8x), Workbench idle unchanged (26.7), Workbench 1.3 boot 60.7 -> ~98 s
      (Kickstart/Workbench boot is CPU-bound in Amiga time). The native
      runner keeps its own 16-cycle loop (regression baselines unchanged).
- [x] **Copper vertical compare is 8-bit.** WAITs compare VP7-VP0 only, so a
      low-byte wait after the line-255 wait completes on lines 256+ (RemGame's
      PAL bottom bands were late); a WAIT fetched still on line 255 is held
      until the wrap.
- [x] **DIWSTOP ninth bit.** OCS takes VSTOP8 = NOT VSTOP7 (stop lines
      $80-$17f); Omega forced it to 1, so RemGame's NTSC window (DIWSTOP $ea
      = line 234) never closed and drew past its bottom. Full-width LORES
      screens are now centred in the frame (a fixed 32-row offset cut the
      last 32 lines of 256-line PAL screens) and DIWSTRT-anchored LORES lines
      above line 44 are drawn (RemGame's NTSC window opens at line 34).
- [ ] **Boot time at the real CPU ratio.** Kickstart 1.3 + Workbench 1.3
      now needs ~98 s (was ~61 s at 16 cycles/slot); faster emulation of
      CPU-bound work, or a turbo option for booting, would win it back.
- [ ] **Optional frame skip.** Don't render/clear frames that will not be
      presented (e.g. render 1 of N); make N a build or runtime option.
- [x] **Pixel conversion on core 1.** Core 0 enqueues bitplane blocks,
      palette changes and frame begin/end into an 8 KB SPSC ring; core 1
      converts between its line interrupts. PAL 320 MHz: boot to the
      Workbench icons 53.9 → 48.0 s; idle Workbench 23.0 → 26.9 emulated
      vblanks/s (~17.5 frames/s shown: when a frame is still waiting for the
      display and the ring fills first, core 1 skips drawing one). All of
      core 1's thread code must stay in RAM (see README).
- [x] **Larger ring.** 32 KB on NTSC, 16 KB on PAL (all other SRAM is free:
      nothing uses malloc since PicoDVI left). Idle Workbench shown/emulated
      frames: NTSC 22.1/32.0 → 27.1/31.9, PAL 17.5/26.8 → 18.6/26.9; boot
      unchanged (~47 s). SRAM left: ~8.1 KB NTSC, ~3.9 KB PAL.
- [x] **32 KB ring on PAL; skip only when the ring is full.** The DF0 track
      moved to PSRAM (uncached, streamed MFM encode, bit-identical tracks)
      and the linker heap to its 256-byte minimum, which frees the 16 KB.
      PAL SRAM left: 16 bytes (NTSC ~21 KB). Core 1 now waits for the swap
      until the ring is full instead of 3/4 full. Idle Workbench
      emulated/shown, KS1.3+WB1.3: PAL 29.8/20.1 → 29.4/20.5, NTSC
      35.5/24.5 → 35.3/~26.8; boot 44.1 → 45.1 s (PSRAM track).
- [x] ~~**Show the rest of PAL's frames.**~~ Done by the 64 KB ring (above).
      Former notes: A PAL Workbench frame is ~4× the
      ring, so no affordable ring absorbs the wait for the 50 Hz swap. Never
      skipping (`OMEGA_RING_SKIP_WORDS=RING_WORDS`) gives a steady 25/25
      but costs ~15 % emulation speed. Needs SRAM freed by the frames
      themselves (smaller frames, or frames in PSRAM). Note: the former
      default "skip when full" (`RING_WORDS`) never triggered, because the
      ring stops filling just short of full; the default is now
      `RING_WORDS - 17`.
- [x] **Bitplane modulo on blank lines.** Omega added BPLxMOD on no-fetch
      lines outside the display window, so a game loading BPLxPT at the top
      of the frame (RemGame: HAM, interleaved, modulo 212, display from $2c)
      showed every plane 44 lines of modulo too far on — garbage HAM colours
      on both the old and the new path. Lines outside DIW's vertical range
      now add nothing; inside it the compatibility advancement stays
      (Kickstart 2.x/3.x load pointers mid-line before no-fetch lines and the
      layout is calibrated for it). Native regressions unchanged.

### P2 – Emulation core
Profile on the Kickstart 1.3 hand screen (NTSC, after the RGB332 render; the
animation keeps the 68000 busy): `m68k_execute` 15.6 %, `dma_execute` 10.8 %,
`evenCycle` 9.4 %, `chipReadLong` 9.2 %, `chipReadByte` 5.0 %, opcode handlers
~15 %, `hostRasterWritten` 4.4 %. The emulator is CPU-bound there (~4.4 TOD
ticks/s, ~7 % speed). After a KEY insert Kickstart needs ~7 s of Amiga time
(trackdisk's change polling) to notice the disk, which is why AmigaDOS
appears only ~2 min 12 s later.
- [x] ~~**Hot code in SRAM.**~~ Tried 2026-09-28 and reverted: ~6 KB of the
      hottest functions (DMA slots, planar conversion, Copper, CIA,
      `m68k_execute`, chip RAM accessors) placed in `.time_critical` put 95 %
      of PC samples in SRAM code but gave no speedup (idle 5.9 vblanks/s, boot
      152 s). The XIP cache was already serving the code; the time is spent
      waiting on PSRAM *data*. It also exhausted PAL's SRAM (PicoDVI's TMDS
      `malloc` failed until the buffers were made static). Revisit only after
      P1, and for opcode handlers only if the CPU becomes the bottleneck.
- [x] **Chip RAM fast path + ROM-first instruction fetch.** `chipRead*` /
      `chipWrite*` test chip RAM first and use one load/store plus REV
      instead of byte accesses; Musashi's prefetch uses `chipFetch*`
      (ROM first) via `M68K_SEPARATE_READS` on Pico builds.
- [x] **CPU slices** (now `OMEGA_CPU_CYCLES_PER_SLOT`, see above). The main
      loop runs `m68k_execute(16 * N)` after every
      N DMA slots (`OMEGA_CPU_SLICE_SLOTS`, default 4) instead of
      `m68k_execute(16)` per slot. Same CPU:DMA cycle ratio; the CPU sees
      chipset state at N-slot (~1.1 µs) granularity. Watch beam-racing
      software; set the constant to 1 to restore per-slot interleaving.
      Results (NTSC, KEY pressed with the hand screen up 30 s → AmigaDOS
      window): 116.1 s → 93.5 s (memory/fetch) → 78.8 s (slices). PAL boot to
      the Workbench icons: 93.5 s → 86.8 s.
- [x] **Per-slot overhead, part 1.** Display-window, fetch-window end and
      plane masks are cached per line behind a dirty flag set by the
      DIW/DDF/BPLCON0/DMACON handlers; VPOSR is set once per line; the E-clock
      counter is inlined; `dma_run(n)` batches slots with the line-end and
      fetch-end work in separate functions. Native frames bit-identical.
      PAL 320 MHz: boot (first AmigaDOS/WB frame) 64.7 → 52.9 s, idle 17.9 →
      22.9 vblanks/s.
- [x] **Per-slot overhead, part 2.** Per-line cached bitplane window,
      full-width flag, hires top row and row rotation; `evenCycle` skips
      `copperExecute()` while the Copper waits (`copperWaitReached()`),
      `oddCycle` skips the blitter when it is idle. Native frames
      bit-identical. KS1.3+WB1.3, 320 MHz: boot to the Workbench icons ~47 →
      44.1 s; idle emulated vblanks/s PAL 26.9 → 29.8, NTSC 31.9 → 35.5.
      Faster emulation fills the ring sooner, so NTSC shown fell 27.1 → 24.5
      until the skip point moved to a full ring (above).
- [x] **Per-slot overhead, part 3: idle-slot skip, 8-slot CPU slices.**
      `dma_run()` jumps over slots that would do nothing while the Copper
      waits and the blitter is idle (`slotNext[]`, rebuilt from
      `dmaUpdateLineState()` when its inputs change; disabled planes' latches
      are cleared there). Identical native frames with and without it.
      `OMEGA_CPU_SLICE_SLOTS` default 4 → 8 (16/32 break RemGame). NTSC,
      5 cycles/slot: xSysinfo drawn 28.8 → 21.6 s, Workbench boot 64.3 →
      51.6 s, idle 31.7 → 34.5 (xSysinfo) and 31.6 → 35.6 (Workbench).
      Remaining idle cost is mostly real work: hires bitplane slots, sprite
      slots (sprite DMA on), the per-slot loop and CIA stepping.
- [x] **Hot emulator code in SRAM.** `chipFetchLong()` looked costly in
      the xSysinfo draw, but 63 % of its samples were the stalled ROM load,
      not the address decode: Kickstart code missed the 16 KB XIP cache that
      flash, PSRAM and the emulator's own code share (1.3 M misses/s while
      drawing, ~25 % of core 0). Flash already runs at ~107 MHz (QMI clkdiv
      3). `OMEGA_HOT_CODE_IN_RAM` (default ON) moves DMA.c, CIA.c, Blitter.c,
      Floppy.c, m68kcpu.c, Memory.c and Host.c to SRAM via a linker-script
      override (~28.7 KB; SRAM free PAL ~28 KB, NTSC ~48 KB). Misses while
      drawing 1.33 → 0.67 M/s; xSysinfo drawn 21.6 → 18.9 s, Workbench boot
      51.6 → 47.5 s, RemGame 16.0 → 21.1 fps; idle −4 %. Only the 68000-side
      files in RAM: 20.3 s / 49.5 s / 18.5 fps, idle unchanged.
- [x] **Batched core-1 messages.** Consecutive bitplane blocks of a row go
      to core 1 as one HIRES/LORES message of up to 16 blocks (count in
      header bits 24..27), flushed before palette, sprite and frame messages.
      The largest message is now 49 words (ring skip point and core 1's
      wrap copy follow it).
- [x] **Batched CIA timers.** `CIAClock()` counts E-clock ticks and runs
      `CIAExecute()` only at the next event tick (`ciaHorizon`: underflow,
      forced load, IRQ to raise); `CIASync()` applies pending ticks before
      CIA register access and external ICR events. Identical native frames
      and logs. With the batched messages, NTSC 5 cycles/slot: xSysinfo
      drawn 18.5 → 17.5 s, Workbench boot 47.5 → 44.3 s, idle 33.2 → 35.4
      (xSysinfo) and 34.0 → 36.0 (Workbench), RemGame 21.1 → 21.8 fps.
- [x] **Fewer per-slot costs in `dma_run()`.** Profiling `main` showed the
      loop itself at 35-56 % of core 0 (Workbench idle 56 %, xSysinfo draw
      and RemGame ~35 %), the 68000 only 5-24 %. VHPOSR is now stored once
      per call (the Copper's SKIP reads the beam directly), and fetch-window
      stretches with an idle Copper and blitter run their active slots back
      to back (`slotRunEnd[]`). Calling every slot of a stretch was slower
      (disabled-plane slots went through the Copper/blitter checks); walking
      `slotNext[]` inside it is the gain. Identical native frames and logs
      (8- and 64-slot A/B). NTSC 5 cycles/slot: xSysinfo drawn 17.5 →
      15.4 s, Workbench boot 44.3 → 36.9 s, idle 35.4 → 43.8 (xSysinfo) and
      36.0 → 48.0 (Workbench), RemGame 21.8 → 24.8 fps.
- [x] **Block runs written in place.** Runs are collected in the ring past
      `ring_head` and published by advancing it (staging buffer only near
      the ring's end). In-place writing alone gave ~1 %: the inlined
      palette upload and staged push made `hostDirectHires()` save 9
      registers and set up a 100-byte frame per block; those paths are now
      out of line. NTSC 5 cycles/slot: xSysinfo drawn 15.4 → 15.2 s, idle
      43.8 → 45.0 (xSysinfo) and 48.0 → 49.6 (Workbench), RemGame 24.8 →
      25.1 fps.
- [ ] **Unreproduced core-0 hard fault.** Seen three times (2026-10-01,
      2026-10-04 twice), each after a `disk.sh` reset with SWD sampling
      around; the UART report never arrived, and seven targeted attempts did
      not reproduce it. `hardfault_report()` now stores pc, lr, CFSR, HFSR,
      MMFAR, BFAR, sp and xPSR in PSRAM (boot state + 0x10, magic
      0xfa017bad) before printing; the next boot prints "Previous
      HARDFAULT", and the record is readable over SWD.
- [x] **CIA timer B in "count timer A underflows" mode never underflowed.**
      `CIAExecute()` tested `tb == 65535` after the decrement, but `tb` is
      signed and reaches -1, so the timer ran on below zero without reloading
      or interrupting. Now tests -1 like the other modes, and INMODE 11 (TA
      underflows while CNT is high) counts too, as CNT is not emulated and
      idles high. None of the regression boots, xSysinfo or RemGame use the
      mode (frames unchanged); checked with a CIA.c test harness (TA latch 9,
      TB latch 2: TB interrupt on the third TA underflow, tick 30, same with
      per-tick and batched clocking).
- [x] **Hot 68000 opcode handlers in SRAM.** Region sampling showed 16-22 %
      of core 0 still in flash (opcode handlers 8-17 %, `main()` and its
      veneers ~3 %, flash copies of `m68ki_read_imm_16`/`m68ki_get_ea_ix`),
      XIP misses 0.4-0.7 M/s. `OMEGA_HOT_OPCODES_IN_RAM` (default ON) puts
      the 100 hottest handlers (`omega/m68khot.h`, ~13 KB, 81 % of handler
      flash time; generated by `tooling/hotops.py` from Workbench boot,
      xSysinfo and RemGame sessions), those helpers and `emulation_loop()`
      in SRAM (~15 KB; free SRAM now PAL ~12 KB, NTSC ~32 KB).
      `m68k_execute()` looks each opcode up once (cycles taken before the
      call). NTSC 5 cycles/slot: xSysinfo drawn 15.2 → 13.7 s, Workbench
      boot 36.9 → 33.0 s, idle 45.0 → 46.2 (xSysinfo) and 49.6 → 51.1
      (Workbench), RemGame 25.1 → 26.1 fps.
- [x] **Instruction fetch page table.** `chipFetchLong` was ~9 % of core 0
      in RemGame: ~5.5 % slow-RAM fetches walking the register range checks
      (RemGame runs from slow RAM; only ROM and chip RAM had fast paths),
      ~2.6 % load stalls, mostly XIP hit latency (misses 0.33 M/s). Fetches
      now use `fetch_page[]` (one pointer per 64 KB page). The same lookup
      in front of data reads/writes cost the idle screens ~1 % (register
      accesses) without helping, so data accesses keep their checks. An
      SRAM chip-RAM cache is not worth it (needs invalidation on CPU,
      blitter and disk writes for <2.6 %). RemGame 26.1 → 27.6 fps,
      xSysinfo drawn 13.7 → 13.5 s; idle and boot within ±1 % build-layout
      noise.
- [x] **No slow RAM (2026-10-03).** 2 MB chip RAM is enough for this
      project and most Amigas had no trapdoor/Ranger RAM. `0xC00000`-
      `0xD7FFFF` now mirrors the custom registers every 512 bytes (reads and
      writes), as on an A500 without it; `0xD80000`-`0xD9FFFF` reads 0. The
      native build (which treated all of `0xC00000`-`0xD9FFFF` as RAM) does
      the same; only the `wb13` baselines changed (title bar: 3,519,632 →
      1,946,800 bytes free). Board: Workbench 1.3, xSysinfo (1.99 MB chip,
      fast RAM 0) and RemGame (27.7 fps, now from chip RAM) run as before.
      PSRAM `0x200000`-`0x27FFFF` is free.
- [x] **Stopped CPU and inline instruction fetch (2026-10-03).** A fresh
      profile of `main` (bd6513e) showed idle screens calling `m68k_execute()`
      every slice with the 68000 in STOP (entry/exit ~4 %), and the prefetch
      calling `chipFetchLong()` per 32-bit code word. `m68k_execute()` now
      tests STOP before saving registers (loop in `m68k_execute_running()`),
      and the prefetch reads `chip_fetch_page[]` inline
      (`chipFetchLongInline()`, ~2 KB more SRAM code; PAL free ~10 KB).
      NTSC 5 cycles/slot: xSysinfo drawn 13.0 → 12.8 s, Workbench boot 31.9
      → 31.0 s, idle 45.5 → 47.1 (xSysinfo) and 50.6 → 52.4 (Workbench),
      RemGame 27.7 → 27.9 fps.
- [x] **Fetch runs without per-slot calls (2026-10-03).** The profile of
      `main` (32cbdd1) showed each bitplane word going through an indirect
      slot call, `bitplaneActive()`, `markBitplaneFetched()`, and for plane 1
      the block setup plus a call into `Host.c`. Fetch runs now fetch enabled
      planes 2-6 inline (`slotPlane[]`), call `hiresPlane1Fetch()` /
      `loresPlane1Fetch()` without the window test, and append blocks that
      continue the current run inline (`src/HostRing.h`). Identical native
      frames and logs (8/64-slot A/B). NTSC 5 cycles/slot: xSysinfo drawn
      12.8 → 12.3 s, idle 47.1 → 54.0 (xSysinfo) and 52.4 → 60.0 (Workbench,
      full speed), RemGame 27.9 → 30.0 fps. Left in a run: chip RAM load
      stalls (~11 %), the per-slot `slotNext`/`switch` step (~10 %, would
      need whole fetch groups across slice cuts), `dmaIdleUntil()` per
      8-slot slice (~11 %).
- [x] **Prefetch word select (2026-10-03).** `m68ki_read_imm_16()` takes
      the word as `(pc & 2) ? low : high` (identical results, ~6 machine
      instructions fewer per 68000 instruction). xSysinfo drawn 12.3 → 12.1
      s, Workbench boot 30.1 → 29.5 s; RemGame unchanged (30.0 fps).
      Finding: 99 % of RemGame's 68000 instructions are its wait-for-line
      loop at 0x1F64C (VPOSR/VHPOSR polling, 12 instructions).
- [x] **Copper idle horizon cached (2026-10-04)**: `dmaIdleUntil()` per line,
      invalidated by Copper/blitter activity at the horizon, line end, CPU
      custom register writes and reset; check inlined. Exact (A/B). Idle
      xSysinfo +4 %, Workbench boot −6 %, RemGame +1.4 %.
- [x] **Real-time limit (2026-10-04)**: `OMEGA_REALTIME`, emulation locked to
      the display refresh (credit capped at two frames); idle Workbench had
      reached 61.6 emulated frames/s. Boots now take real time.
- [ ] **Remaining exact chipset items** (each small, RemGame estimates):
      whole fetch groups without the per-slot `switch` (~2-4 %);
      single-colour palette messages instead of 32 colours per Copper
      colour write (~1 %); skip sprite slots of idle/finished sprites
      (~1 %).
- [ ] **Idea: beam-wait fast-forward (speed hack, not exact).** Recognise a
      loop that only polls VPOSR/VHPOSR against a target line and run the
      chipset to that line instead of interpreting it (RemGame ~30 → ~40
      fps). Exits up to one loop iteration (~150 CPU cycles) off the exact
      point; RemGame's beam racing may break. Only as an off-by-default
      option with visual checks.
- [ ] **Idea: dynamic hot opcode cache in SRAM.** Adapt the SRAM handler set
      to the running software instead of a build-time list:
      1. Sampling: at each line end `dma_run()` counts the handler of the
         last 68000 instruction (`REG_IR`), ~15.7 k samples/s at a few
         cycles each (core 1 cannot read core 0's DWT PC sampler).
      2. About once a second, copy the hottest handlers not yet in SRAM into
         a fixed arena (e.g. 12 KB), point `m68ki_handler_ptrs[]` at the
         copies (core 0 between instructions, so no locking; no I-cache on
         SRAM, a DSB/ISB is enough) and evict the ones that cooled off.
      3. Copies must be position independent: the handlers call out with
         PC-relative `bl` (`cpu_read_*`, `chipFetchLong`, fetch helpers), so
         build the handler files with `-mlong-calls` and verify at build time
         that a handler has no PC-relative reference outside itself (switch
         tables are internal; only 2 handlers have them).
      The static list already covers 81 % of the handler flash time for the
      tested software (2-4 % of core 0 left), so the gain is for software
      with a different hot set, and PAL could trade SRAM for speed at
      runtime. `-mlong-calls` slows every handler call slightly. Start with
      the line-end sampler alone to see how much the hot set differs between
      programs.
- [x] ~~**Idea: cold opcode handlers in PSRAM.**~~ Not worth it: flash and PSRAM
      share the 16 KB XIP cache and the QMI bus at the same clock, and a
      PSRAM miss is no cheaper (flash can use continuous-read mode; PSRAM
      needs a command per access and also carries chip RAM traffic).
- [x] **Audio DMA never ran** (`chipset.dmacon` vs `dmaconr`): fixed with
      Paula (Phase 5).
- [ ] **SRAM window over low chip RAM** (vectors, stacks, hot Exec data) once
      SRAM is available; one compare per access saves a QSPI round trip.

### P3 – Clocks and memory
- [x] **HSTX DVI.** Hardware TMDS encoding frees core 1 (it only services a
      per-line DMA interrupt) and removed PicoDVI's TMDS line buffers (~44 KB
      free heap on NTSC, ~23 KB on PAL).
- [x] **Decoupled clk_sys.** HSTX runs from `PLL_USB`; `clk_sys` is the
      `OMEGA_SYS_CLK_KHZ` build option, default 320 MHz (PSRAM 107 MHz). PAL
      boot (first AmigaDOS/WB frame): 270/90 MHz 82.1 s → 320 MHz 66.7 s;
      240 MHz 88.2 s, 266 MHz 79.4 s, 300 MHz 71.5 s. Long-term stability at
      320 MHz / 1.20 V not yet soak-tested. USB needs an external 48 MHz
      clock (GPIN0/GPIO20) or a multiple-of-48 `clk_sys`.
- [ ] **Soak-test 320 MHz.** A KS 2.04 run once derailed the 68k (illegal
      exception with a bogus supervisor stack) and not again in a 5 min
      rerun; overclock or PSRAM timing is suspected. The resulting host
      hard fault is fixed: word/long writes beyond 0xDFF1FF no longer index
      past `putChipReg16/32`, and long reads beyond the 16-entry
      `getChipReg32` return 0.
- [x] **Frame pacing.** The emulator no longer waits for the display
      (idle Workbench had been capped at 50/3 vblanks/s). The first version
      took back unshown frames, which could stop frames from ever being
      shown; the core-1 version never takes a finished frame back (see the
      pixel conversion item).
- [ ] **Kickstart in PSRAM vs flash.** ROM fetches come from flash XIP;
      measure whether a PSRAM copy (126 MHz) is faster.

### P4 – Floppy / boot time
Measured on a PAL Workbench 1.3 boot (170 s SWD timeline, 2026-09-28): the
emulator runs at a steady ~5.2 vblanks/s throughout, the same as idle, so the
whole boot is only ~16 s of Amiga time (a real A500 needs ~1 min). The floppy
emulation is already ~4× faster than real hardware; loading is slow in wall
time because the emulator runs at ~10 % speed. Disk DMA is active ~20 % of the
boot, and `floppyDataRead` + `diskCycle` cost only ~4 % of core 0 while it
runs. The display pipeline dominates during loading too (~50 %), so P1 is also
the main boot-time fix. Note: the OS rewrites CIA-A TOD during boot; sum only
forward steps when measuring.
- [ ] **Faster disk DMA — needs care.** `turboFloppy` 8 → 64 words per slot cut
      the board's boot to the Workbench icons from 151.5 s to 132.7 s, but the
      native PAL `wb13` case then never reaches Workbench (blank screen even at
      vbl 2522). Find what breaks (sync search, DSKBLK timing vs trackdisk)
      before raising it; at most ~20 % of boot time is at stake.
- [x] ~~**Floppy on core 1.**~~ Not worth it (measured 2026-09-29, PAL
      320 MHz, disk-heavy boot phase 8-35 s after reset, 135k PC samples): all
      floppy code is 2.0 % of core 0 (`floppyDataRead` 1.0 %, `diskCycle`
      0.8 %, drive state 0.1 %); MFM track encoding does not register. Disk
      data must also be answered within the emulated DMA slot, so moving it
      would cost more in synchronisation than it saves. Boot time follows the
      emulator speed (`dma_run` 19 %, bitplane slots ~22 %, `m68k_*` ~20 %).

### Done
- [x] Musashi opcode pointer table → 16-bit handler index, descriptor table in
      flash, per-handler cycle counts (~210 KB SRAM freed; verified identical
      for all 65,536 opcodes).
- [x] Profiling method established (see above).

---

## Known issues / investigation needed

- [x] **`chipset.vposr` PAL/NTSC flag**: `dma_execute()` uses
      `OMEGA_VIDEO_VPOSR_ID` from `omega/VideoStandard.h` (`0x0000` PAL,
      `0x1000` NTSC Fat Agnus), selected by `OMEGA_VIDEO_MODE`. xsysinfo
      reports NTSC / FatAgnus 8370 on the NTSC build.
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

- [x] **Slow RAM shadow fixed (2026-09-03).** KS 3.14 early-boot code accesses
      chipset registers through their slow-RAM mirror (e.g. reads INTENAR via
      `A2-0xFE4` where A2=0xC20000 → effective address 0xC1F01C).  The low 12
      bits of such addresses map to chipset register offsets (0x01C = INTENAR,
      0x01E = INTREQR).  `native/memory_native.c chipReadWord` now checks
      `(addr & 0xFFF) < 0x20` in the slow-RAM path and redirects to
      `getChipReg16[]` for the 16 readable word registers, leaving genuine
      slow-RAM reads untouched.  The unsafe original `#ifdef NOSLOWRAM` path
      (which had no bounds check and would OOB-index into `getChipReg16`) is
      superseded by this targeted fix.
      Superseded (2026-10-03): slow RAM is gone and `0xC00000`-`0xD7FFFF`
      mirrors all custom registers in both `src/Memory.c` and the native
      build (see "No slow RAM").
- [ ] **Stack size**: Musashi uses recursion for instruction dispatch. Default Pico
      stack (2 KB) may be too small. Add `pico_set_binary_type(omega-amiga copy_to_ram)`
      or increase stack in linker script if crashes occur.
- [ ] **Blitter accuracy**: Omega blitter is "immediate mode" (fires all at once).
      Works for most software but some demos rely on cycle-accurate blitter timing.
      Low priority.
