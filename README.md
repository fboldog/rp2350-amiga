# Omega Amiga Emulator – RP2350 Bare-Metal Port

Port of [Omega](https://github.com/h5n1xp/Omega) (bare-metal 68K + Amiga chipset emulator)
to the **RP2350B**, with build support for the Waveshare RP2350-PiZero and
WeAct Studio RP2350B Core. Normal builds use the default 150 MHz clock; HDMI
builds use the 252 MHz NTSC or 270 MHz PAL PicoDVI serializer clock and retime
PSRAM through the Pico SDK.

## Status

| Component | Status |
|---|---|
| Build system (Pico SDK 2.x) | ✅ |
| PSRAM initialization | ✅ Pico SDK `hardware_psram` startup path |
| PSRAM validation | ✅ 8 MB detected and deterministic read/write test passed on WeAct |
| Chip RAM / Slow RAM in PSRAM | ✅ implemented |
| ROM from microSD, with flash fallback | ⏸ implemented, build-disabled by default |
| Musashi 68K CPU core | ✅ |
| Custom chipset + CIA + DMA | ✅ |
| Floppy (DF0 raw flash ADF + SRAM track cache) | ✅ verified; build-disabled by default |
| Framebuffer in PSRAM | ✅ implemented |
| HDMI output | ✅ verified on WeAct; build-disabled by default |
| USB HID keyboard/mouse | ⬜ Phase 2 |

## Hardware

All board wiring is in [`src/board_config.h`](src/board_config.h). The Waveshare
Pico SDK board header is local; current Pico SDK releases provide the WeAct
header.

- **MCU**: RP2350B (80-pin, GPIO0..47), Cortex-M33
- **PSRAM**: 8 MB mapped at `0x11000000`; CS is GPIO47 on Waveshare and GPIO0
  on WeAct. The WeAct board is populated; Waveshare requires a compatible chip
  soldered to its optional U1 footprint.
- **Flash**: 16 MB QSPI (CS0) → firmware at `0x10000000`, ROM at `0x10200000`
- **Debug console**: UART1 TX=GPIO4/RX=GPIO5 at 115200 on WeAct; UART0
  TX/RX=GPIO0/1 at 115200 on Waveshare. Connect the debug probe's RX to
  GPIO4, TX to GPIO5, and join GND.
- **Display**: PicoDVI via PIO0. WeAct uses Raspberry Pi's Pico DVI Sock
  ordering on GPIO12..19: D0=12/13, CLK=14/15, D2=16/17, D1=18/19.
  Waveshare uses its on-board connector: D0=36/37, D1=34/35, D2=32/33,
  CLK=38/39. Both mappings use `invert_diffpairs=false`. If the WeAct output
  has stable blue/yellow false colors, reverse the D0 pair rather than changing
  the color-channel mapping in software.
- **microSD**: SPI1 — SCK=GPIO30, MOSI=GPIO31, MISO=GPIO40,
  CS=GPIO43 (4-bit SDIO also wired: D0=40, D1=41, D2=42, D3=43, CLK=38).
- **USB**: native USB-C (TinyUSB host for HID), plus an optional PIO-USB port.
- **LED**: one WS2812 on GPIO2 (no plain LED).

## PSRAM Layout

```
0x11000000  Chip RAM (2 MB)
0x11200000  Slow/Ranger RAM (512 KB)
0x11280000  Legacy/full-image DF0 MFM region (unused by streamed DF0)
0x11480000  SD Kickstart cache (512 KB) / DF1 MFM buffer (2 MB)
0x11500000  Waveshare HDMI RGB332 scanout buffers (inside DF1 region)
0x11540000  Video DMA raster scratch (640×400 ARGB32)
0x11680000  Framebuffer 640×400 ARGB32 (1 MB)
0x11780000  Reserved
```

## Flash Layout

```
0x10000000  Firmware (≤ 2 MB)
0x10200000  Kickstart ROM (256 KB mirrored to 512 KB, or 512 KB)
0x10280000  DF0 ADF image (raw 901120-byte image)  [optional]
0x10480000  DF1 ADF image (raw 901120-byte image)  [optional]
```

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
# Waveshare (default)
cmake -S . -B build -G Ninja
cmake --build build -j

# WeAct Studio RP2350B Core
cmake -S . -B build-weact -G Ninja -DPICO_BOARD=weact_studio_rp2350b_core
cmake --build build-weact -j

# WeAct with HDMI on GPIO12..19
cmake -S . -B build-weact-hdmi -G Ninja \
  -DPICO_BOARD=weact_studio_rp2350b_core \
  -DOMEGA_ENABLE_HDMI=ON \
  -DOMEGA_ENABLE_FLASH_FLOPPY=ON \
  -DOMEGA_DF0_INSERT_AT_BOOT=ON
cmake --build build-weact-hdmi -j
```

HDMI and SD-card reading are temporarily disabled by the `OFF` defaults of
`OMEGA_ENABLE_HDMI` and `OMEGA_ENABLE_SDCARD`. They are excluded from the
source and link dependency lists, not merely skipped at runtime.
Flash-backed ADF loading is also disabled by default for bring-up; enable it
with `-DOMEGA_ENABLE_FLASH_FLOPPY=ON` after HDMI is stable. By default the
mounted DF0 starts ejected and the WeAct KEY button inserts it. Add
`-DOMEGA_DF0_INSERT_AT_BOOT=ON` to boot the disk without a button press; KEY
then ejects or reinserts the mounted image.

The HDMI build raises `clk_sys` to PicoDVI's serializer clock (252 MHz for
NTSC or 270 MHz for PAL) and then retimes the SDK-managed PSRAM. The local
PicoDVI compatibility patch drives a clock pair that starts on an odd GPIO from
synchronised adjacent PWM slices.
Full-width emulator modes convert the native DMA raster directly to RGB332
scanout frames, avoiding a redundant 640×400 ARGB presentation pass.

The DVI mode is standard: 640×480p60 (VGA, 25.2 MHz pixels, matching the
252 MHz NTSC clock) or CEA 720×576p50 for PAL (270 MHz). Each stored image row
is sent twice and black lines fill the remaining height. Firmware panics at
startup if `clk_sys` does not match the mode's bit clock: PicoDVI's 720×480p60
timing at 252 MHz produced an off-standard ~55.9 Hz mode that capture devices
locked onto unreliably.

WeAct scans out at full width without upscaling: each of the 640 image
columns becomes one DVI pixel, so HIRES (e.g. Workbench text) keeps every
pixel. Core 1 drives the RP2350 SIO TMDS encoder at one symbol per pixel.
Its two 640×200 RGB332 frames live in internal SRAM, not PSRAM: the emulator
saturates the shared QMI bus, and 720-byte PSRAM (or flash) line fetches
regularly missed the scanline deadline, which PicoDVI shows as solid red lines.
Nothing in core 1's per-line path may execute from or read flash for the same
reason. Four TMDS buffers suffice because the encoder no longer stalls.

Waveshare keeps the pixel-doubled 320×200 path: its double-buffered RGB332
frames share DF1's PSRAM region, so HDMI builds keep DF1 disabled (DF0 remains
available), core 1 stages one line in SRAM, and eight TMDS buffers absorb
short QMI stalls.

The DVI buffers show color bars during the first 1.5 seconds, then are
overwritten by emulator video.
Core 0 waits for a core-1 acknowledgement after `dvi_start()` before beginning
the emulator's heavy PSRAM traffic, preventing intermittent cold-start loss of
the DVI signal.

The firmware defaults to NTSC timing. Select PAL at configure time with
`cmake .. -G Ninja -DOMEGA_VIDEO_MODE=PAL`. The selected standard controls the
chipset frame length, video-identification bit, and display viewport. PAL uses
the same complete 400-row intermediate raster used by NTSC.

Output: `<build-directory>/omega-amiga.uf2`

### PSRAM startup

Pico SDK `hardware_psram` detects and configures PSRAM before `main()`. The
firmware then uses the same basic validation as the `rp2350b-psram` project.
Expected UART output on WeAct is:

```text
Omega/RP2350 – Amiga emulator
Sys clock: 252000 kHz
Video: NTSC (263 lines, 59.940 Hz)
PSRAM: detected 8388608 bytes on GPIO0
PSRAM: 1024-byte read/write test passed at 0x11000000
ROM: flash 0x10200000, header=1111 opcode=4ef9 entry=00fc00d2
DVI: PIO 640x480p60 full-width 640x400 emulator scanout on GPIO12..19 (SRAM frames)
DF0: flash ADF mounted with 12798-byte SRAM track cache
DF0: disk inserted at boot; KEY ejects/reinserts it
Entering emulation loop
DVI: first direct-raster emulator frame queued
```

### Load Kickstart from microSD

This Waveshare-only path is currently excluded from the default build. Configure
with `-DOMEGA_ENABLE_SDCARD=ON` to include it while testing SD separately.

Format a microSD card as FAT16 or FAT32 and copy the project `sd_card`
contents to its root. The default ROM path is therefore:

```text
/rom/kick13.rom
```

At boot the firmware mounts the card through SPI1 and loads a valid 256 KB or
512 KB Kickstart into PSRAM before resetting the 68K. Change
`BOARD_SD_ROM_PATH` in `src/board_config.h` to select another ROM. If the card,
filesystem, or file cannot be read, the firmware uses the ROM embedded in flash.

The SD ROM cache overlaps DF1, so DF1 is disabled when an SD ROM is active;
DF0 remains available. Flash-backed DF0 keeps its raw ADF in flash and encodes
only the active 12,798-byte MFM track into internal SRAM. ADF files are not yet
read from SD, but that path can reuse the same track cache once sector reads
replace the current memory-mapped flash source.

### Flash with Kickstart ROM

You need a legally-owned Kickstart ROM (KS 1.3 or KS 2.x recommended).

```bash
# Combine firmware + ROM into a single UF2
python3 tools/combine_uf2.py \
    build-weact-hdmi/omega-amiga.uf2 \
    sd_card/rom/kick13.rom \
    build-weact-hdmi/omega-amiga-kick13.uf2

# Optionally add ADF floppy images
python3 tools/combine_uf2.py \
    build-weact-hdmi/omega-amiga.uf2 \
    sd_card/rom/kick13.rom \
    build-weact-hdmi/omega-amiga-kick13-wb13.uf2 \
    --adf0 sd_card/adf/amiga-os-134-workbench.adf
```

ADF embedding remains optional. The combined Kickstart 1.3 + Workbench 1.3
image was verified on WeAct with DF0 inserted at boot, and with KEY eject and
reinsert events. A 256 KB Kickstart is mirrored into its 512 KB flash window by
the combine tool.

Hold BOOTSEL and connect USB, then copy the generated combined UF2 to the
`RPI-RP2` drive.

## Display output

PicoDVI output is implemented for both supported boards. Other possible output
backends remain future work:

| Output | Library | Notes |
|---|---|---|
| DVI/HDMI | [PicoDVI](https://github.com/Wren6991/PicoDVI) | Implemented and verified on WeAct (full width) |
| VGA | pico-vga-scanvideo | Needs resistor ladder, 3 GPIO per channel |
| SPI TFT | st7789 / ili9341 | Easy hardware, 320×240 typical |

The raw DMA raster is at `PSRAM_BASE + PSRAM_VIDEO_RASTER_OFFSET`. HDMI
full-width modes convert it directly into the RGB332 scanout buffers;
narrow/wrapped modes retain the 640×400 ARGB presentation fallback at
`PSRAM_BASE + PSRAM_FRAMEBUF_OFFSET`. Logical Amiga scanlines and LORES
overscan positioning match the native test runner.

## Phase 2: USB HID

Wire `pressKey()`/`releaseKey()` to TinyUSB HID keyboard events.
Joystick/mouse delta goes into `chipset.joy0dat` (see original Host.c).
TinyUSB is included in the Pico SDK; add `tinyusb_host` to `target_link_libraries`.

## Architecture Notes

- **Memory.c is fully replaced** by `src/memory.c` which uses PSRAM via address
  translation (Amiga addresses → PSRAM offsets).  The original 16 MB static
  array is gone.
- **Host.c is fully replaced** by `src/host.c`.  All SDL2 code is removed.
- **Musashi 68K** (`omega/m68k*.c`) keeps only the 68000 tables in Pico builds
  to reduce internal SRAM use; the native build retains the full tables.
  Pico builds also replace the 256 KB opcode pointer table with a 16-bit
  handler index per opcode (128 KB) and keep the opcode description table in
  flash, freeing about 150 KB of SRAM for the WeAct HDMI frames.
- **Chipset/CIA/DMA/Blitter** remain close to upstream, with Pico memory paths,
  quieter diagnostics, and the shared raster-position fixes called out above.
- **Floppy.c** has a `PICO_BUILD` guard: the desktop `ADF2MFM(fd,...)` retains
  full-image conversion, while RP2350 DF0 uses a one-track SRAM MFM cache over
  the raw flash ADF.
- **CPU.c** has a `PICO_BUILD` guard in `cpu_pulse_reset()` to call
  `memory_clear_chipram()` instead of the `low16Meg` loop.
- **FatFs** is configured read-only and provides optional FAT16/FAT32 ROM
  loading on Waveshare. It is only compiled with `OMEGA_ENABLE_SDCARD=ON`.

## License

Omega is licensed under MPL 2.0.  RP2350 port additions are also MPL 2.0.
Kickstart ROMs are © Commodore/Cloanto – you must own a legal copy.
FatFs is distributed under its own permissive license in
`third_party/fatfs/LICENSE.txt`.
