# Omega Amiga Emulator - RP2350 Port

Port of [Omega](https://github.com/h5n1xp/Omega) (bare-metal 68K + Amiga chipset emulator)
to the **RP2350B**, targeting the [**WeAct Studio RP2350B Core**](https://github.com/WeActStudio/WeActStudio.RP2350BCoreBoard) + [Adafruit DVI breakout](https://www.adafruit.com/product/4984). 

My plan was using the [Waveshare RP2350B RP2350-PiZero](https://www.waveshare.com/rp2350-pizero.htm), but unfortunately this board HDMI output 
not connected to the HSTX pins. This is forces the DVI/HDMI output do the work from SW side, which is from 
emulation performance isn't the best direction.

Normal builds use the default 150 MHz clock; HDMI builds overclock `clk_sys` to
`OMEGA_SYS_CLK_KHZ` (default 320 MHz) and retime PSRAM through the Pico SDK.

## Status

| Component | Status |
|---|---|
| Build system (Pico SDK 2.x) | ✅ |
| PSRAM initialization | ✅ Pico SDK `hardware_psram` startup path |
| PSRAM validation | ✅ 8 MB detected and deterministic read/write test passed |
| 2 MB chip RAM in PSRAM (no slow RAM) | ✅ implemented |
| ROM from microSD, with flash fallback | ⏸ implemented, build-disabled by default (needs an external SD module) |
| Musashi 68K CPU core | ✅ |
| Custom chipset + CIA + DMA | ✅ |
| Floppy (DF0 raw flash ADF + PSRAM track buffer) | ✅ verified; build-disabled by default |
| HDMI output (HSTX) | ✅ verified NTSC and PAL; build-disabled by default |
| USB HID keyboard/mouse | ⬜ Phase 2 |

## Hardware

All board wiring is in [`src/board_config.h`](src/board_config.h); the WeAct
board header ships with the Pico SDK.

- **MCU**: RP2350B (80-pin, GPIO0..47), Cortex-M33
- **PSRAM**: 8 MB mapped at `0x11000000`, CS GPIO0 (populated on the board)
- **Flash**: 16 MB QSPI (CS0) → firmware at `0x10000000`, ROM at `0x10200000`
- **Debug console**: UART1 TX=GPIO4/RX=GPIO5 at 115200. Connect the debug
  probe's RX to GPIO4, TX to GPIO5, and join GND.
- **Display**: HDMI through the RP2350 HSTX peripheral on GPIO12..19, in
  Raspberry Pi's Pico DVI Sock ordering: D0=12/13, CLK=14/15, D2=16/17,
  D1=18/19.
- **KEY button** (GPIO23): inserts/ejects the DF0 disk.
- **microSD** (optional, external module): SPI1 defaults SCK=GPIO30,
  MOSI=GPIO31, MISO=GPIO40, CS=GPIO43, overridable with `-DBOARD_SD_*_PIN`.

## PSRAM Layout

8 MB at `0x11000000` (cached XIP window; `0x15000000` is the uncached alias of
the same memory). Offsets are defined in `src/board_config.h`
(`BOARD_MAP_*`).

| Address | Size | Content | Access |
|---|---|---|---|
| `0x11000000` | 2 MB | Chip RAM, Amiga `0x000000`–`0x1FFFFF` | cached |
| `0x11200000` | 512 KB | free (was slow RAM) | |
| `0x11280000` | 2 MB | DF0 MFM buffer: the active track of the flash ADF, or the empty-drive image when no ADF is in flash | uncached |
| `0x11480000` | 512 KB | SD Kickstart cache (SD-card builds only) | cached |
| `0x11500000` | 256 KB | free | |
| `0x11540000` | 1000 KB | Video DMA raster, 640×400 ARGB32 (fallback path for layouts the direct HDMI path cannot draw) | cached |
| `0x1163A000` | 280 KB | free | |
| `0x11680000` | 1 MB | Presented framebuffer, 640×400 ARGB32 (same fallback path) | cached |
| `0x11780000` | 32 KB | HDMI frame records: palette log and pixel runs of the two indexed frames (`dvi_indexed_frame_t` ×2) | cached |
| `0x11788000` | 128 KB | 68000 opcode table scratch: the flat handler index, built at boot and then compressed into SRAM | boot only |
| `0x117A8000` | ~348 KB | free | |
| `0x117FF000` | 12 bytes | DF0 disk rotation state (kept through resets, lost on power-off) | uncached |

The HDMI scanout frames themselves are in internal SRAM, not PSRAM.

The emulated Amiga has 2 MB of chip RAM and no slow ("trapdoor"/Ranger) RAM,
like most machines. As on an A500 without it, `0xC00000`–`0xD7FFFF`
mirrors the custom registers every 512 bytes (Kickstart's memory sizing
tells the mirror from RAM by writing INTENA through it; Kickstart 3.1 also
reads INTENAR there during early boot). Workbench 1.3 shows 1,946,800 bytes
free; xSysinfo reports 1.99 MB chip and no fast RAM.

## SRAM Layout

520 KB of internal SRAM: 512 KB main SRAM at `0x20000000` plus two 4 KB
banks, `SCRATCH_X` (`0x20080000`) and `SCRATCH_Y` (`0x20081000`). Sizes are
from the Release builds (`arm-none-eabi-nm -S`); PAL / NTSC where they
differ.

| Region | Size | Content |
|---|---|---|
| Vector table | 272 B | RAM copy of the vector table |
| `.data` | ~58 KB | Code and tables copied to RAM: core 1's conversion loop and HSTX interrupt (anything core 1 runs must not fetch from flash), the chipset register dispatch tables, `DMALores`/`DMAHires` slot tables, with `OMEGA_HOT_CODE_IN_RAM` (default) all code of `DMA.c`, `CIA.c`, `Blitter.c`, `Floppy.c`, `m68kcpu.c`, `Memory.c` and `Host.c` (~28.7 KB), and with `OMEGA_HOT_OPCODES_IN_RAM` (default) the 100 hottest opcode handlers, their fetch helpers and the main loop (~15 KB) |
| `sram_frames` | 320 KB / 300 KB | Two scanout frames, 640×256 (PAL) / 640×240 (NTSC), one byte per pixel (Amiga colour number or HAM code; RGB332 on the fallback path) |
| `ring` | 64 KB | Core 0 → core 1 message ring (bitplane blocks, palette changes, sprites) |
| `m68ki_opcode_blocks` + `m68ki_opcode_block` | 32 KB + 1 KB | Two-level 68000 opcode → handler index: room for 256 shared blocks of 64 entries (~245 used), one byte per block of opcodes |
| `m68ki_handler_ptrs` + `m68ki_handler_cycles` | 7.7 KB + 3.8 KB | Handler function pointers and 68000 cycle counts, one per handler |
| `image_lines` | 5.1 KB | Two RGB888 row buffers with HSTX commands, expanded by the scanout interrupt |
| `c2p_spread` | 2 KB | Planar-to-chunky lookup table (core 1) |
| `rgb332_rgb888` | 1 KB | RGB332 → RGB888 table (boot pattern, fallback frames) |
| `chip_fetch_page` | 1 KB | 68000 instruction-fetch page table: host pointer per 64 KB page |
| `hostRasterRowMin/Max` | 1.6 KB | Written column range per raster row (ARGB fallback path) |
| Other `.bss` | ~2.5 KB | Chipset and CPU state, scanout palette, HAM tables, SDK state |
| Heap | 256 B | Minimum SDK heap (nothing calls `malloc`) |
| **Free** | **~12 KB / ~32 KB** | Between the heap and the end of main SRAM (+15 KB with `-DOMEGA_HOT_OPCODES_IN_RAM=OFF`, +28.7 KB more with `-DOMEGA_HOT_CODE_IN_RAM=OFF`) |
| `SCRATCH_X` | 4 KB | Core 1 stack (2 KB) and 2 KB free |
| `SCRATCH_Y` | 4 KB | Core 0 stack |

The core 0 stack grows down from the top of `SCRATCH_Y`; an overrun would run
into `SCRATCH_X` without a guard. The HDMI colour records, the DF0 track and
the boot-time opcode table scratch were moved to PSRAM to make room for the
64 KB ring (see the PSRAM table above). A third scanout frame (160 KB PAL /
150 KB NTSC) does not fit.

## Flash Layout

16 MB at `0x10000000`.

| Address | Size | Content |
|---|---|---|
| `0x10000000` | ≤ 2 MB | Firmware (currently ~330 KB) |
| `0x10200000` | 512 KB | Kickstart ROM (256 KB images are mirrored to 512 KB) |
| `0x10280000` | 880 KB each | DF0 ADF images, up to 15 back to back (`combine_uf2.py --adf …`); the first slot starting with erased flash ends the list |

Only DF0 is used. DF1–DF3 appear to Kickstart as unconnected drives.

## Building

### Prerequisites

```bash
sudo apt install cmake gcc-arm-none-eabi
```

If `PICO_SDK_PATH` is unset, CMake downloads Pico SDK 2.3.1 into the build
directory. Git and an internet connection are required on the first configure.
Use an existing checkout instead with `-DPICO_SDK_PATH=/path/to/pico-sdk`, or
select another release with `-DPICO_SDK_GIT_TAG=<tag>`.

### Configure and build

```bash
# Emulator without HDMI
cmake -S . -B build -G Ninja
cmake --build build -j

# HDMI, DF0 from flash, disk inserted at boot (NTSC)
cmake -S . -B build-weact-hdmi -G Ninja \
  -DOMEGA_ENABLE_HDMI=ON \
  -DOMEGA_ENABLE_FLASH_FLOPPY=ON \
  -DOMEGA_DF0_INSERT_AT_BOOT=ON
cmake --build build-weact-hdmi -j

# The same for PAL
cmake -S . -B build-weact-hdmi-pal -G Ninja -DOMEGA_VIDEO_MODE=PAL \
  -DOMEGA_ENABLE_HDMI=ON -DOMEGA_ENABLE_FLASH_FLOPPY=ON \
  -DOMEGA_DF0_INSERT_AT_BOOT=ON
cmake --build build-weact-hdmi-pal -j
```

`PICO_BOARD` defaults to `weact_studio_rp2350b_core`, the only supported
board. HDMI, SD-card reading and flash-backed ADF loading are `OFF` by
default; disabled subsystems are excluded from the build, not merely skipped
at runtime. By default the mounted DF0 starts ejected and the KEY button
inserts it. Add `-DOMEGA_DF0_INSERT_AT_BOOT=ON` to boot the disk without a
button press; KEY then ejects or reinserts the mounted image.

The firmware defaults to NTSC timing. Select PAL with
`-DOMEGA_VIDEO_MODE=PAL`. The selected standard controls the chipset frame
length, video-identification bit, display viewport and the HDMI mode.

Output: `<build-directory>/omega-amiga.uf2`

### HDMI output

`src/dvi_display.c` drives the RP2350 **HSTX** peripheral. HSTX TMDS-encodes
RGB888 pixels (one per 32-bit word) in hardware; its command lists generate
syncs and porches, and `TMDS_REPEAT` commands fill the borders. Two ping-pong
DMA channels feed it one scanline at a time. Core 1 services one DMA
interrupt per line: it re-arms the finished channel first and then expands
the next image row into one of two RGB888 row buffers, which is sent on both
of the row's lines. Because the line commands never change, a late expansion
could at worst tear a row, never lose sync; swaps happen at line 0.

HSTX has its own clock: `PLL_USB` is retuned to the TMDS bit rate (252 MHz
NTSC, 270 MHz PAL) and `clk_hstx` is `PLL_USB / 2`, since HSTX outputs two
bits per cycle. `clk_sys` is therefore independent of the video mode and set
at build time with `-DOMEGA_SYS_CLK_KHZ=<kHz>` (default 320000; PSRAM runs at
`clk_sys / ceil(clk_sys / 133 MHz)`, 107 MHz at 320 MHz). `clk_peri` (UART,
SPI) moves to `PLL_SYS` divided to ≤150 MHz, and the USB/ADC clocks are
stopped: USB will need an external 48 MHz clock on GPIN0 (GPIO20) or a
`clk_sys` that is a multiple of 48 MHz. Firmware panics at startup if
`PLL_USB` does not match the mode.

PAL boot benchmark (first AmigaDOS/Workbench frame, Kickstart 1.3 +
Workbench 1.3): 240 MHz 88.2 s, 266 MHz 79.4 s, 300 MHz 71.5 s, 320 MHz
66.7 s (64.7 s with the frame pacing below); the old 270 MHz with 90 MHz
PSRAM took 82.1 s. The CPU clock matters more than the PSRAM clock.

The pixel conversion runs on core 1. Core 0 (the emulator) only enqueues
each fetched bitplane block, palette changes and frame begin/end into a
single-producer/single-consumer ring of 32-bit words; core 1 converts them
into the frame between its line interrupts (`hostCore1Loop()` in
`src/Host.c`). The ring is 64 KB in both modes (`RING_WORDS`; any size
works, positions run over twice the ring size), which holds a whole frame's
messages across the wait for the display swap: on idle Workbench every
emulated frame is shown (PAL 28.0 of 28.0 frames/s, was 21.7 of 28.2 with
24 KB and 21.9 with 32 KB; NTSC 33.6 of 33.6, was 32.5 with 44 KB; larger
rings change nothing). Consecutive bitplane blocks of one row travel as a
single message of up to 16 blocks, sent before any palette change, sprite
or frame boundary, so core 0 pushes once per run and core 1 dispatches once
per run instead of per block. Core 0 collects a run in place in the ring
just past `ring_head` and publishes it by advancing `ring_head`; only a run
that could reach the ring's end goes through a staging buffer. Core 1 reads
each message in place and copies out only one that may wrap. Everything core 1
executes must live in RAM: a flash fetch stalled behind core 0's PSRAM
traffic once delayed the line interrupt past its deadline and stopped the
DMA chain.

The frame handshake is one atomic word (displayed buffer + pending flag);
core 1 switches to the pending frame at line 0. A finished frame is never
taken back. If it has not been shown when the next frame starts, core 1
waits for the swap (≤ one display frame) while core 0 keeps queueing, and
skips drawing that frame once core 0 can no longer queue the largest message
(49 words: a run of 16 lores blocks), so the emulator barely waits for the
display.
`OMEGA_RING_SKIP_WORDS` sets the skip point; `RING_WORDS` never skips and
locks emulation to the display cadence (PAL idle Workbench 25.0/25.0
emulated/shown frames/s instead of 29.4/20.6). `dvi_frames_shown` counts the frames actually shown.

The DVI mode is standard: 640×480p60 (VGA, 25.2 MHz pixels, 252 MHz) for
NTSC or CEA 720×576p50 (270 MHz) for PAL. Each stored image row is sent
twice. Like a real Amiga, every area around the image (the PAL side columns
and the rows above and below) shows the border colour COLOR00, never black: a
solid bright boot screen framed by black side columns made the capture device
lose lock during Kickstart's grey/white boot sequence. An earlier 720×480p60
mode at 252 MHz ran off-standard at ~55.9 Hz, which capture devices locked
onto unreliably.

The image is 640 pixels wide without upscaling, so HIRES (e.g. Workbench
text) keeps every pixel. Its two 640×240 (NTSC) or 640×256 (PAL) frames live
in internal SRAM, not PSRAM: the emulator saturates the shared QMI bus.

Colours are exact. The frames hold one byte per pixel: the Amiga colour
register number (0..31, 32..63 extra half-brite) or, on HAM runs, the raw
6-bit HAM code. Core 1 converts each bitplane block to those numbers
(`hostDirectHires()`/`hostDirectLores()`, HAM included) and keeps a record
per frame (`dvi_indexed_frame_t`): the palette at frame begin, a log of every
colour change (up to 1024), and for each row the runs of pixels it drew with
the log position before them (up to 1024). The records live in PSRAM at
0x780000 (32 KB reserved), read and written through the XIP cache that both
cores share, which keeps ~9.6 KB of SRAM free for the ring at ~1 % speed. Scanout replays the log in drawing
order and expands each row to RGB888 with the 12-bit colours doubled to 8 bits
(0xF → 0xFF), so Copper palette changes (the Kickstart 2.04 rainbow, border
gradients) keep their exact colours even mid-line; a row's undrawn columns
and the PAL side borders show that row's COLOR00. HAM holds its colour across
the row from COLOR00, as on real hardware. A full log drops further changes
for the rest of that frame; the next frame starts from the full palette.
Narrow fetch layouts use the same path with the presentation mapping; only
layouts that need the wrapped-fetch reconstruction fall back to the ARGB
raster in PSRAM, shown as RGB332.

The frames show colour bars for the first 1.5 seconds, then emulator video.
HSTX starts a few seconds after reset, once core 0 has validated PSRAM and
filled the boot pattern.

### PSRAM startup

Pico SDK `hardware_psram` detects and configures PSRAM before `main()`. The
firmware then uses the same basic validation as the `rp2350b-psram` project.
Expected UART output is:

```text
Omega/RP2350 – Amiga emulator
Sys clock: 252000 kHz
Video: NTSC (263 lines, 59.940 Hz)
PSRAM: detected 8388608 bytes on GPIO0
PSRAM: 1024-byte read/write test passed at 0x11000000
ROM: flash 0x10200000, header=1111 opcode=4ef9 entry=00fc00d2
DVI: HSTX 640x480p60 emulator scanout on GPIO12..19
DF0: flash ADF 1 of 1 (0x10280000) mounted with a 12798-byte PSRAM track buffer
DF0: disk inserted at boot; KEY ejects/reinserts it
Entering emulation loop
DVI: first indexed emulator frame queued
```

### Load Kickstart from microSD

This path is excluded from the default build and needs an external SPI SD
module (the WeAct board has no slot). Configure with `-DOMEGA_ENABLE_SDCARD=ON`
and set the `BOARD_SD_*` pins to match the wiring.

Format a microSD card as FAT16 or FAT32 and copy the project `sd_card`
contents to its root. The default ROM path is therefore:

```text
/rom/kick13.rom
```

At boot the firmware mounts the card through SPI1 and loads a valid 256 KB or
512 KB Kickstart into PSRAM before resetting the 68K. Change
`BOARD_SD_ROM_PATH` in `src/board_config.h` to select another ROM. If the card,
filesystem, or file cannot be read, the firmware uses the ROM embedded in flash.

Flash-backed DF0 keeps its raw ADF in flash and encodes only the active
12,798-byte MFM track, streamed straight into the DF0 area of PSRAM. The
track is written and read through the uncached XIP alias, so disk loading
does not evict emulator code or chip RAM from the shared 16 KB XIP cache
(boot costs ~1 s more than the former SRAM track, which is now part of the
core-1 ring). ADF files are not yet read from SD, but that path can reuse the
same track buffer once sector reads replace the current memory-mapped flash
source.

### Flash with Kickstart ROM

You need a legally-owned Kickstart ROM (KS 1.3 or KS 2.x recommended).

```bash
# Combine firmware + ROM into a single UF2
python3 tools/combine_uf2.py \
    build-weact-hdmi/omega-amiga.uf2 \
    sd_card/rom/kick13.rom \
    build-weact-hdmi/omega-amiga-kick13.uf2

# Optionally add DF0 ADF images (repeat --adf to rotate between them)
python3 tools/combine_uf2.py \
    build-weact-hdmi/omega-amiga.uf2 \
    sd_card/rom/kick13.rom \
    build-weact-hdmi/omega-amiga-kick13-wb13.uf2 \
    --adf sd_card/adf/amiga-os-134-workbench.adf
```

**Disk rotation (demo mode).** Up to 15 ADF images (880 KB each, flash
0x280000 onwards on 16 MB) can be embedded with repeated `--adf` options.
Every boot, a reset included, mounts the next one in DF0; a power cycle
starts again at the first. The position is kept in PSRAM (offset
0x7FF000, uncached, guarded by a magic and a complement): the PSRAM stays
powered through a RUN-pin reset but loses its contents on a power cycle,
and flash is never written at runtime. (POWMAN scratch registers were tried
first: they survive SWD resets but are cleared by the RESET button.) The tool writes an erased marker after the last image, so images
left over from an earlier, longer list are not mounted. The boot log names
the disk, e.g. `DF0: flash ADF 2 of 3 (0x1035c000) mounted ...`.

ADF embedding remains optional. The combined Kickstart 1.3 + Workbench 1.3
image was verified with DF0 inserted at boot, and with KEY eject and reinsert
events. A 256 KB Kickstart is mirrored into its 512 KB flash window by the
combine tool. The tool numbers the firmware and appended blocks as one UF2
sequence: the RP2350 boot ROM reboots once it has received the number of
blocks announced by the first block, so separately numbered ROM/ADF blocks
were silently never written and the previous disk stayed in flash.

Hold BOOTSEL and connect USB, then copy the generated combined UF2 to the
`RPI-RP2` drive.

## CPU timing

The 68000 gets `OMEGA_CPU_CYCLES_PER_SLOT` cycles per DMA slot (colour clock),
default 2 as on an A500 (7.09 MHz CPU, 3.55 MHz colour clock); set it at
configure time with `cmake -DOMEGA_CPU_CYCLES_PER_SLOT=<n>` (a cached value
wins over the default, so reconfigure existing build directories). Musashi's
overrun past a slice's budget is carried into the next slice so the ratio is
exact. Omega used 16, a 68000 running ~8x fast against the beam: software
that races the beam (RemGame refills its sprite bands behind the beam) then
misbehaves. At the real ratio, busy-wait loops cost 8x less host time (RemGame
~9 -> ~18 frames/s) but CPU-bound work such as booting Kickstart and
Workbench takes its real Amiga time (Workbench 1.3 boot ~98 s instead of
~61 s). Chip-bus wait states are not modelled. The native runner keeps its
own 16-cycle loop. Board helper scripts are in `tooling/` (frame capture,
speed, boot timing, DF0 slot selection, profiling).

The 68000 runs in slices of `OMEGA_CPU_SLICE_SLOTS` DMA slots (default 8,
`-DOMEGA_CPU_SLICE_SLOTS=<n>`): longer slices enter Musashi less often, but
the CPU then sees the beam move in coarser steps. 8 is the longest that keeps
RemGame's beam racing correct; 16 and 32 break its sprite bands.

`dma_run()` jumps over runs of DMA slots that would do nothing: while the
Copper waits for a position it has not reached (or is off) and the blitter is
idle, slots that only offer themselves to the Copper or blitter (including
those of disabled bitplanes, sprite slots with sprite DMA off, and bitplane
slots outside the fetch window) are skipped in one step, ticking only the
E clock. The result is identical to stepping each slot (native frames
match with and without it, per slot and in 8-slot batches);
`-DOMEGA_SLOT_SKIP=OFF` disables it.

Measured on the board (NTSC, 5 cycles/slot): xSysinfo boot-to-drawn 28.8 →
21.6 s, Workbench 1.3 boot 64.3 → 51.6 s, idle xSysinfo 31.7 → 34.5 and
Workbench 31.6 → 35.6 frames/s (skip plus 8-slot slices vs neither).

The emulator's hottest code runs from SRAM (`OMEGA_HOT_CODE_IN_RAM`, default
ON): a linker-script override (`src/ld/default_text_excludes.incl`) keeps
`DMA.c`, `CIA.c`, `Blitter.c`, `Floppy.c`, `m68kcpu.c`, `Memory.c` and
`Host.c` out of flash `.text`, so the SDK copies their code (~28.7 KB) to
SRAM at boot. Flash and PSRAM share one 16 KB XIP cache, and with the
chipset loop in flash, Kickstart ROM code (e.g. graphics.library while
xSysinfo draws) kept missing it: misses while drawing fell from 1.33 to
0.67 M/s. Board, NTSC, 5 cycles/slot: xSysinfo drawn 21.6 → 18.9 s,
Workbench 1.3 boot 51.6 → 47.5 s, RemGame 16.0 → 21.1 fps; idle screens
lose ~4 % (xSysinfo 34.5 → 33.2, Workbench 35.6 → 34.0 fps), as that code
was already cached and now shares SRAM with core 1 and the HDMI DMA. The
opcode handler files (65-88 KB each) stay in flash, except for the hottest
handlers below.

The 100 most used opcode handlers (~13 KB of ~300 that run; 81 % of the
handler time spent in flash) also run from SRAM (`OMEGA_HOT_OPCODES_IN_RAM`,
default ON), with the fetch helpers they call and the main loop
(`emulation_loop()`): `omega/m68khot.h` redeclares them with
`__not_in_flash_func`, so the generated handler files are unchanged. The
list comes from PC samples on the board (Workbench 1.3 boot, xSysinfo
drawing, RemGame); `tooling/hotops.py` samples other software and
regenerates it. `m68k_execute()` also looks each opcode up once, taking
its cycles before calling the handler, instead of repeating the two-level
lookup after it. Board, NTSC, 5 cycles/slot: xSysinfo drawn 15.2 → 13.7 s,
Workbench 1.3 boot 36.9 → 33.0 s, idle xSysinfo 45.0 → 46.2 and Workbench
49.6 → 51.1 frames/s, RemGame 25.1 → 26.1 fps. Placing the cold handlers
in PSRAM instead of flash would not help: both sit behind the same XIP
cache and QMI bus, and a PSRAM miss is no cheaper.

Instruction fetches look up a 256-entry page table (`chip_fetch_page[]` in
`src/Memory.c`, one host pointer per 64 KB page: chip RAM, ROM with its
mirror; NULL elsewhere). Before, only ROM and chip RAM had a fast path,
and RemGame, which then ran from slow RAM, walked the register range
checks on every fetch. Data accesses keep their range checks; a lookup in
front of them slowed register accesses. RemGame 26.1 → 27.6 fps, xSysinfo
drawn 13.7 → 13.5 s; an SRAM cache for chip RAM would not pay off (load
stalls ~2.6 % of core 0 in RemGame, XIP misses 0.33 M/s).
Musashi's prefetch reads through that table inline
(`chipFetchLongInline()` in `src/Memory.h`), without a call, and
`m68k_execute()` checks for STOP before saving any register (idle screens
leave the 68000 stopped while the main loop still calls it every slice):
xSysinfo drawn 13.0 → 12.8 s, Workbench 1.3 boot 31.9 → 31.0 s, idle
xSysinfo 45.5 → 47.1 and Workbench 50.6 → 52.4 frames/s, RemGame 27.7 →
27.9 fps.

The CIA timers are clocked in batches (`CIAClock()` in `omega/CIA.h`). Most
E-clock ticks only count the running timers down, so they are just counted;
the full `CIAExecute()` runs only on the tick where something happens (a
timer underflow, a forced load, an interrupt to raise), and the counted
ticks are applied before every CPU access to a CIA register and before a
keyboard, disk-index or TOD-alarm event. Timer values and interrupt timing
are unchanged (identical native frames and logs). Together with the
batched bitplane messages above, on the board (NTSC, 5 cycles/slot):
xSysinfo drawn 18.5 → 17.5 s, Workbench 1.3 boot 47.5 → 44.3 s, idle
xSysinfo 33.2 → 35.4 and Workbench 34.0 → 36.0 frames/s, RemGame 21.1 →
21.8 fps.

Inside the bitplane fetch window the same condition (Copper and blitter
idle up to a slot) lets `dma_run()` run a whole stretch of bitplane and free
slots back to back (`slotRunEnd[]`): it calls only the stretch's active slot
functions (`slotNext[]`) and does the per-slot bookkeeping (E clock, beam
position, skip checks) once for the stretch; `-DOMEGA_SLOT_SKIP=OFF` turns
this off too. VHPOSR is likewise stored once per `dma_run()` call, from the
last slot run: only the CPU reads it between calls, and the Copper's SKIP
compares against the beam directly. Native frames and logs are unchanged
per slot and in 8- and 64-slot batches. Board, NTSC, 5 cycles/slot:
xSysinfo drawn 17.5 → 15.4 s, Workbench 1.3 boot 44.3 → 36.9 s, idle
xSysinfo 35.4 → 43.8 and Workbench 36.0 → 48.0 frames/s, RemGame 21.8 →
24.8 fps.

Writing block runs straight into the ring, with the producers' rare paths
(palette upload, staged push, run start) kept out of line so each block
costs no large stack frame or register saves, adds: xSysinfo drawn 15.4 →
15.2 s, idle xSysinfo 43.8 → 45.0 and Workbench 48.0 → 49.6 frames/s,
RemGame 24.8 → 25.1 fps (Workbench boot unchanged at 36.9 s).

Inside a fetch run the window test of the bitplane slot functions always
holds, so `dma_run()` fetches enabled planes 2-6 inline (`slotPlane[]`,
built with `slotRunEnd[]`) and calls `hiresPlane1Fetch()` /
`loresPlane1Fetch()` (the slot bodies without the test) for plane 1. A block
that continues the current core-1 run with the palette unchanged is
appended inline (`hostRunContinue()` in `src/HostRing.h`); anything else
takes `hostDirectHires/Lores()`, so the ring contents are unchanged. Native
frames and logs match per slot and in 8- and 64-slot batches. Board, NTSC,
5 cycles/slot: xSysinfo drawn 12.8 → 12.3 s, idle xSysinfo 47.1 → 54.0 and
Workbench 52.4 → 60.0 frames/s (full NTSC speed: emulation now waits for
the display), RemGame 27.9 → 30.0 fps. What remains in a run is mostly the
chip RAM loads of the bitplane words (PSRAM through the XIP cache).

The prefetch picks the instruction word with `(pc & 2) ? low : high`
instead of shift arithmetic, about 6 machine instructions fewer per 68000
instruction: xSysinfo drawn 12.3 → 12.1 s, Workbench 1.3 boot 30.1 →
29.5 s. RemGame (30 fps) spends almost all of its 68000 time in a
wait-for-raster-line loop (`move.w $dff004/$dff006` polling), which exact
emulation has to interpret.

## Sprites

`omega/DMA.c` implements OCS sprite DMA. Sprite n owns two DMA slots per
line (0x15 + 4n, 0x17 + 4n): it fetches SPRxPOS/SPRxCTL on the first line
after vertical blanking, SPRxDATA/SPRxDATB on every line from VSTART, and
the next POS/CTL pair at VSTOP, so a sprite can be reused further down the
screen. Words go through the register handlers, so DATA arms and CTL disarms
a sprite exactly as CPU writes do (manual sprites work too), and the Copper
can reposition sprites mid-frame (RemGame moves six sprites every 16 lines).
Unused slots stay free for the blitter.

At the end of each line's fetch window the armed sprites are drawn over the
line: lower sprites in front, clipped to the display window, attached pairs
as 15-colour sprites (each half at its own position, combined where they
coincide), and BPLCON2 decides which pairs sit behind playfield colours
other than 0 (single playfield only). On the board core 1 overlays them on
the indexed frame as colour registers tagged with 0x40, so a sprite on a HAM
row neither takes nor changes the held HAM colour; the native build draws
them on the ARGB raster. Sprite collisions (CLXDAT) and dual playfield are
not emulated.

## Display pipeline

The raw DMA raster is at `PSRAM_BASE + PSRAM_VIDEO_RASTER_OFFSET`. HDMI
full-width and narrow modes (HAM included) render straight to the indexed
SRAM frames; wrapped-fetch modes use the raster and the
640×400 ARGB presentation fallback at `PSRAM_BASE + PSRAM_FRAMEBUF_OFFSET`.
Logical Amiga scanlines and LORES overscan positioning match the native test
runner.

## Phase 2: USB HID

Wire `pressKey()`/`releaseKey()` to TinyUSB HID keyboard events.
Joystick/mouse delta goes into `chipset.joy0dat` (see original Host.c).
TinyUSB is included in the Pico SDK; add `tinyusb_host` to `target_link_libraries`.

## Code map

### What runs where

| Where | What | Code |
|---|---|---|
| Core 0, main loop | The emulator: DMA slot by slot (bitplanes, sprites, Copper, blitter, disk, audio), the 68000 in slices between them, CIA timers, frame end | `src/main.c` → `dma_run()` in `omega/DMA.c`, `m68k_execute()` in `omega/m68kcpu.c` |
| Core 0, per bitplane block | Queues the block (or a palette change, sprite, frame begin/end) for core 1 | `hostDirect*()` in `src/Host.c` |
| Core 1, thread | Converts queued blocks to colour numbers in the free scanout frame, overlays sprites, logs palette changes | `hostCore1Loop()` in `src/Host.c` |
| Core 1, DMA interrupt (once per line) | Re-arms the HSTX DMA chain, swaps frames at line 0, expands the next image row to RGB888 (palette log replay, HAM decode) | `hstx_dma_irq()` / `scan_expand_row()` in `src/dvi_display.c` |
| DMA + HSTX hardware | TMDS-encodes and sends each line; no CPU involvement | configured in `src/dvi_display.c` |

Everything core 1 runs is in RAM (`__not_in_flash_func`): a flash fetch
stalled behind core 0's PSRAM traffic once made the line interrupt miss its
deadline.

### `src/` — RP2350 platform layer

| File | Does |
|---|---|
| `main.c` | Startup: clocks (`clk_sys`, PLL_USB for HSTX, PSRAM timing), PSRAM check, Kickstart lookup, DF0 disk selection and rotation (state in PSRAM), HDMI start, then the emulation main loop with the CPU:DMA cycle ratio (`OMEGA_CPU_CYCLES_PER_SLOT`); hard-fault report |
| `board_config.h` | All board pins (PSRAM, HDMI, SD, KEY button), feature defaults and the PSRAM/flash memory map (`BOARD_MAP_*`) |
| `Memory.c` / `Memory.h` | The 68000's view of memory: 2 MB chip RAM in PSRAM, the custom register mirror at `0xC00000`–`0xD7FFFF`, ROM in flash (or the SD cache), CIA and custom-register dispatch, bounds checks so a runaway program cannot crash the host |
| `psram.c` / `psram.h` | PSRAM detection check and read/write test; `psram_ptr()` and region offsets |
| `Host.c` / `Host.h` | Host side of the video path: the core 0 → core 1 message ring and producers (`hostDirect*`), core 1's converter and sprite overlay, the ARGB raster fallback, keyboard entry points |
| `HostRing.h` | Core 0 → core 1 message format and the inline fast path for appending a bitplane block to the current run (used by `DMA.c`) |
| `dvi_display.c` / `dvi_display.h` | HDMI over HSTX: video modes, double-buffered SRAM frames, frame handshake, per-line DMA interrupt, row expansion to RGB888 (exact 12-bit colours, HAM), boot colour bars, ARGB/RGB332 submit for the fallback path |
| `Planar.c` | Planar-to-chunky ARGB conversion (HIRES, LORES, HAM) for the fallback raster and the native build |
| `Presentation.c` / `.h` | ARGB fallback presentation: maps the DMA raster to a 640×400 frame, rebuilds wrapped-fetch rows |
| `sd_card.c` / `sd_card.h`, `sd_diskio.c` | SD card over SPI (FatFs disk driver) and Kickstart loading from SD (SD-card builds only) |
| `ld/default_text_excludes.incl` | Linker-script override (`OMEGA_HOT_CODE_IN_RAM`): keeps the hottest emulator files out of flash `.text`, so the SDK places their code in SRAM |

### `omega/` — emulator core (from the Omega project, extended)

| File | Does |
|---|---|
| `DMA.c` / `DMA.h` | The chipset timing loop: one call per DMA slot (colour clock) via the `DMALores`/`DMAHires` slot tables. Bitplane fetch and modulo, display window, sprite DMA and rendering, Copper, blitter/CPU slot sharing, disk and audio DMA, end of line and frame (vertical blank, frame hand-off) |
| `Chipset.c` / `Chipset.h` | Custom-chip registers: read/write dispatch tables (16- and 32-bit), colour registers (palette, EHB, palette log for HDMI), DMACON/INTENA/INTREQ, sprite and bitplane pointers, 32-bit writes split into two 16-bit ones |
| `Blitter.c` / `Blitter.h` | Blitter: area copy with minterms, shifts and masks, line drawing, area fill |
| `CIA.c` / `CIA.h` | The two 8520 CIAs: timers, TOD counters, interrupts, keyboard serial port, disk control lines |
| `Floppy.c` / `Floppy.h` | DF0 drive: motor/step/side, disk change, MFM encoding of the active track from the flash ADF (into PSRAM on the board), the data stream read by disk DMA |
| `CPU.c` / `CPU.h` | Glue to Musashi: memory callbacks, interrupt levels from INTENA/INTREQ, reset |
| `m68k*.c`, `m68k*.h` | Musashi 68000 core. On the RP2350 the opcode table is two-level (`m68kops.c`) and cycle counts are per handler; `m68kdasm.c` is the disassembler (debug only) |
| `m68khot.h` | The hottest opcode handlers to run from SRAM (`OMEGA_HOT_OPCODES_IN_RAM`); generated by `tooling/hotops.py` |
| `Gayle.c` / `Gayle.h` | Gayle/IDE and clock register stubs (writes ignored, reads return fixed values) |
| `DisplayLayout.h` | Display-window and data-fetch decoding shared by DMA and host (DIWSTRT/DIWSTOP, DDFSTRT/DDFSTOP, layout constants) |
| `VideoStandard.h` | PAL/NTSC constants: lines per frame, VPOSR ID, viewport offsets, sprite timing |
| `debug.c` / `debug.h` | Register names for logging, old ADF/MFM helpers |
| `endianMacros.h`, `m68kconf.h`, `Kick13.h` | Byte-order helpers, Musashi configuration, an empty legacy header |

### Other directories

| Path | Does |
|---|---|
| `native/` | Head-less PC runner of the same `omega/` core (`main_native.c`, `host_native.c`, `memory_native.c`), optional SDL window (`display_sdl.c`), KickSmash ROM-switcher simulator, and the framebuffer regression test (`regression.sh` + `regression-baselines.sha256`). See `native/README.md` |
| `tools/combine_uf2.py` | Builds the flashable UF2: firmware + Kickstart + up to 15 DF0 ADFs |
| `tooling/` | Board scripts over SWD: frame capture with colour replay, frame rates, boot timing, disk slot selection, profiling, hot opcode selection. See `tooling/README.md` |
| `image_refs/` | Reference screenshots for visual comparison |
| `sd_card/` | Kickstart ROMs and ADFs used for testing and the SD-card build |
| `third_party/fatfs/` | FatFs, used by the SD-card build |
| `manual-test.sh` | Runs the native build over the Kickstart/Workbench combinations |

## Architecture Notes

- **Memory.c is fully replaced** by `src/Memory.c`, which uses PSRAM via
  address translation (Amiga addresses → PSRAM offsets). The original 16 MB
  static array is gone.
- **Host.c is fully replaced** by `src/Host.c`. All SDL2 code is removed.
- **Musashi 68K** (`omega/m68k*.c`) keeps only the 68000 tables in Pico builds
  to reduce internal SRAM use; the native build retains the full tables.
  Pico builds also replace the 256 KB opcode pointer table with a 16-bit
  handler index per opcode, keep the opcode description table in flash, and
  store 68000 cycle counts per handler instead of per opcode (register
  shifts by an immediate count add their 2-cycles-per-bit cost from the
  opcode); handlers and cycle counts were checked identical for all 65,536
  opcodes against the original tables. The index is two-level: the 1024
  blocks of 64 opcodes (the effective-address field) contain only ~245
  distinct blocks, so a byte per block selects a shared block (~33 KB
  instead of 128 KB). It is built flat in PSRAM scratch at boot, compressed
  and verified against the flat table (panic on mismatch). Together this
  frees about 305 KB of SRAM; the extra lookup costs <1 % idle speed.
- **Chipset/CIA/DMA/Blitter** remain close to upstream, with Pico memory paths,
  quieter diagnostics, and the shared raster-position fixes called out above.
- **Floppy.c** supports DF0 only; DF1–DF3 stay as unconnected drives for
  Kickstart's drive-ID probe. The desktop `ADF2MFM(fd,...)` retains full-image
  conversion, while RP2350 DF0 uses a one-track SRAM MFM cache over the raw
  flash ADF.
- **CPU.c** has a `PICO_BUILD` guard in `cpu_pulse_reset()` to call
  `memory_clear_chipram()` instead of the `low16Meg` loop.
- **FatFs** is configured read-only and provides optional FAT16/FAT32 ROM
  loading. It is only compiled with `OMEGA_ENABLE_SDCARD=ON`.

## License

Omega is licensed under MPL 2.0.  RP2350 port additions are also MPL 2.0.
Kickstart ROMs are © Commodore/Cloanto – you must own a legal copy.
FatFs is distributed under its own permissive license in
`third_party/fatfs/LICENSE.txt`.
