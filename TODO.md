# TODO – Omega RP2350 Port

## Legend
- [x] Done
- [ ] Not started
- [~] In progress / partial

---

## Phase 1 – Compile target (COMPLETE)

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

---

## Phase 2 – Display output

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

### Option C: DVI/HDMI via PicoDVI (best quality, needs HDMI breakout)
- [ ] Wire TMDS pairs (4 GPIO pairs) to HDMI connector (through 270 Ω)
- [ ] Clone PicoDVI and add as submodule / CMake subdirectory
- [ ] Allocate DVI framebuffer (640×480 16bpp = 614 KB) – needs PSRAM
- [ ] In `display_push_frame()`: convert ARGB32 framebuf → RGB565 and hand to PicoDVI
- [ ] Add `libdvi` to target_link_libraries

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

## Known issues / investigation needed

- [ ] **PSRAM timing at 250 MHz**: `src/psram.c` uses `clkdiv=2` (QSPI at 125 MHz).
      Verify APS6404L spec allows 125 MHz at 3.3 V; may need dummy-cycle count tweak.
      Test by reading back a written pattern at startup.
- [ ] **`chipset.vposr` PAL/NTSC flag**: currently hardcoded to NTSC value `0x1000`.
      PAL should be `0x0000`. Change in `omega/DMA.c:dma_execute()`.
- [ ] **ROM validation**: `src/Memory.c:memory_init()` checks `rom_base[0] == 0x11`.
      KS 2.x ROMs start with `0x11 0x14`; KS 1.3 with `0x11 0x11`. Both pass.
      KS 3.x ROMs not supported (Omega limitation, not RP2350 limitation).
- [ ] **Slow RAM shadow**: Omega's original code has `#define NOSLOWRAM` to mirror
      chipset registers at 0xC00000. This is not implemented in `src/Memory.c`.
      Add if KS 1.x boot hangs.
- [ ] **Stack size**: Musashi uses recursion for instruction dispatch. Default Pico
      stack (2 KB) may be too small. Add `pico_set_binary_type(omega-amiga copy_to_ram)`
      or increase stack in linker script if crashes occur.
- [ ] **Blitter accuracy**: Omega blitter is "immediate mode" (fires all at once).
      Works for most software but some demos rely on cycle-accurate blitter timing.
      Low priority.
