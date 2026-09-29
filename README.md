# Omega Amiga Emulator – RP2350 Bare-Metal Port

Port of [Omega](https://github.com/h5n1xp/Omega) (bare-metal 68K + Amiga chipset emulator)
to the **RP2350B**, targeting the **WeAct Studio RP2350B Core**. Normal builds
use the default 150 MHz clock; HDMI builds overclock `clk_sys` to
`OMEGA_SYS_CLK_KHZ` (default 320 MHz) and retime PSRAM through the Pico SDK.

## Status

| Component | Status |
|---|---|
| Build system (Pico SDK 2.x) | ✅ |
| PSRAM initialization | ✅ Pico SDK `hardware_psram` startup path |
| PSRAM validation | ✅ 8 MB detected and deterministic read/write test passed |
| Chip RAM / Slow RAM in PSRAM | ✅ implemented |
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

```
0x11000000  Chip RAM (2 MB)
0x11200000  Slow/Ranger RAM (512 KB)
0x11280000  DF0 MFM buffer (2 MB; empty-drive image when no ADF is in flash)
0x11480000  SD Kickstart cache (512 KB, SD-card builds)
0x11540000  Video DMA raster scratch (640×400 ARGB32)
0x11680000  Framebuffer 640×400 ARGB32 (1 MB)
0x11780000  Reserved
```

## Flash Layout

```
0x10000000  Firmware (≤ 2 MB)
0x10200000  Kickstart ROM (256 KB mirrored to 512 KB, or 512 KB)
0x10280000  DF0 ADF image (raw 901120-byte image)  [optional]
```

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
rings change nothing). Core 1 reads each message in place and copies out
only one that wraps. Everything core 1
executes must live in RAM: a flash fetch stalled behind core 0's PSRAM
traffic once delayed the line interrupt past its deadline and stopped the
DMA chain.

The frame handshake is one atomic word (displayed buffer + pending flag);
core 1 switches to the pending frame at line 0. A finished frame is never
taken back. If it has not been shown when the next frame starts, core 1
waits for the swap (≤ one display frame) while core 0 keeps queueing, and
skips drawing that frame once core 0 can no longer queue the largest message
(17 words), so the emulator barely waits for the display.
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
DF0: flash ADF mounted with a 12798-byte PSRAM track buffer
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

# Optionally add the DF0 ADF image
python3 tools/combine_uf2.py \
    build-weact-hdmi/omega-amiga.uf2 \
    sd_card/rom/kick13.rom \
    build-weact-hdmi/omega-amiga-kick13-wb13.uf2 \
    --adf0 sd_card/adf/amiga-os-134-workbench.adf
```

ADF embedding remains optional. The combined Kickstart 1.3 + Workbench 1.3
image was verified with DF0 inserted at boot, and with KEY eject and reinsert
events. A 256 KB Kickstart is mirrored into its 512 KB flash window by the
combine tool. The tool numbers the firmware and appended blocks as one UF2
sequence: the RP2350 boot ROM reboots once it has received the number of
blocks announced by the first block, so separately numbered ROM/ADF blocks
were silently never written and the previous disk stayed in flash.

Hold BOOTSEL and connect USB, then copy the generated combined UF2 to the
`RPI-RP2` drive.

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
