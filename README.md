# Omega Amiga Emulator – RP2350 Bare-Metal Port

Port of [Omega](https://github.com/h5n1xp/Omega) (bare-metal 68K + Amiga chipset emulator)
to the **RP2350B** (Waveshare RP2350-PiZero), running on Cortex-M33. The
current hardware-debug build uses the RP2350 default 150 MHz clock.

## Status – Phase 1

| Component | Status |
|---|---|
| Build system (Pico SDK 2.x) | ✅ |
| PSRAM detection (APS6404L on QMI CS1) | ✅ 8 MB detected |
| Sustained PSRAM access | ⚠️ blocked during first real write |
| Chip RAM / Slow RAM in PSRAM | ⚠️ implemented, awaiting PSRAM fix |
| ROM from microSD, with flash fallback | ⚠️ implemented, not reached on hardware |
| Musashi 68K CPU core | ✅ |
| Custom chipset + CIA + DMA | ✅ |
| Floppy (DF0/DF1 from flash ADF) | ✅ |
| Framebuffer in PSRAM | ⚠️ implemented, awaiting PSRAM fix |
| Display output (VGA/DVI/SPI-TFT) | ⬜ Phase 2 |
| USB HID keyboard/mouse | ⬜ Phase 2 |

## Hardware

Target board: **Waveshare RP2350-PiZero** (RP2350B).  All board wiring is in
[`src/board_config.h`](src/board_config.h); the Pico SDK board header is
[`src/boards/waveshare_rp2350_pizero.h`](src/boards/waveshare_rp2350_pizero.h).

- **MCU**: RP2350B (80-pin, GPIO0..47), Cortex-M33; currently run at 150 MHz
- **PSRAM**: the tested board has an **APS6404L-3SQR** fitted to U1 on the
  shared QMI bus, CS = GPIO47. Direct identification returns KGD `0x5d`, EID
  `0x46` (8 MB), but sustained memory-mapped access currently stalls. The SDK
  maps CS1 at `0x11000000` after startup detection.
- **Flash**: 16 MB QSPI (CS0) → firmware at `0x10000000`, ROM at `0x10200000`
- **UART**: TX=GPIO0, RX=GPIO1 (115200 8N1) for debug output
- **Display (Phase 2)**: on-board DVI/HDMI.  TMDS D0=GPIO36, D1=GPIO34,
  D2=GPIO32, CLK=GPIO38 (`invert_diffpairs=false`, PIO0, `gpio_base=16`) —
  i.e. PicoDVI's `pico_sock_cfg`.
- **microSD**: SPI1 — SCK=GPIO30, MOSI=GPIO31, MISO=GPIO40,
  CS=GPIO43 (4-bit SDIO also wired: D0=40, D1=41, D2=42, D3=43, CLK=38).
- **USB**: native USB-C (TinyUSB host for HID), plus an optional PIO-USB port.
- **LED**: one WS2812 on GPIO2 (no plain LED).

To build for the original Pimoroni Pico Plus 2 instead, pass
`-DPICO_BOARD=pimoroni_pico_plus2_rp2350` and add `BOARD_*` overrides.

## PSRAM Layout

```
0x11000000  Chip RAM (2 MB)
0x11200000  Slow/Ranger RAM (512 KB)
0x11280000  DF0 MFM floppy buffer (2 MB)
0x11480000  SD Kickstart cache (first 512 KB) / DF1 MFM buffer (2 MB)
0x11680000  Framebuffer 640×400 ARGB32 (1 MB)
0x11780000  Video DMA raster scratch
```

## Flash Layout

```
0x10000000  Firmware (≤ 2 MB)
0x10200000  Kickstart ROM (256 KB mirrored to 512 KB, or 512 KB)
0x10280000  DF0 ADF image (pre-MFM-encoded, ≤ 2 MB)  [optional]
0x10480000  DF1 ADF image (pre-MFM-encoded, ≤ 2 MB)  [optional]
```

## Building

### Prerequisites

```bash
export PICO_SDK_PATH=/path/to/pico-sdk   # SDK 2.x required
sudo apt install cmake gcc-arm-none-eabi
```

### Configure and build

```bash
mkdir build && cd build
cmake .. -G Ninja        # PICO_BOARD defaults to waveshare_rp2350_pizero
ninja
```

The firmware defaults to NTSC timing. Select PAL at configure time with
`cmake .. -G Ninja -DOMEGA_VIDEO_MODE=PAL`. The selected standard controls the
chipset frame length, video-identification bit, and display viewport. PAL uses
the taller 200-line intermediate raster required by its 400-line output.

Output: `build/omega-amiga.uf2`

### Current RP2350 hardware result

The firmware builds and can be flashed with OpenOCD. UART output is available
on GPIO0/GPIO1 at 115200 baud (observed as `/dev/ttyACM1` with the current debug
probe). On the populated APS6404L-3SQR board the current run reaches:

```text
Omega/RP2350 – Amiga emulator
Sys clock: 150000 kHz
Video: PAL (313 lines, 50.000 Hz)
PSRAM: SDK detected 8 MB
```

It then stalls at the first real memory-mapped PSRAM write. A short cached
write/read probe can appear successful and must not be treated as validation.
Until QMI timing/cache behavior is fixed, memory clearing, SD mounting,
Kickstart loading, emulation, and HDMI output are not reached. The HDMI path is
also still a stub, so resolving PSRAM alone will not yet produce video.

### Load Kickstart from microSD

Format a microSD card as FAT16 or FAT32 and copy the project `sd_card`
contents to its root. The default ROM path is therefore:

```text
/rom/kick13.rom
```

At boot the firmware mounts the card through SPI1 and loads a valid 256 KB or
512 KB Kickstart into PSRAM before resetting the 68K. Change
`BOARD_SD_ROM_PATH` in `src/board_config.h` to select another ROM. If the card,
filesystem, or file cannot be read, the firmware uses the ROM embedded in flash.

The SD ROM cache overlaps DF1 because the current full-disk MFM representation
uses almost all 8 MB of PSRAM. Consequently DF1 is disabled when an SD ROM is
active; DF0 remains available. ADF files are not streamed from SD yet: the
RP2350 path still reads an ADF from flash and expands the complete disk into its
2 MB MFM buffer. Direct SD-backed ADF operation will require a track-sized
read/encode cache rather than a normal file pointer.

### Flash with Kickstart ROM

You need a legally-owned Kickstart ROM (KS 1.3 or KS 2.x recommended).

```bash
# Combine firmware + ROM into a single UF2
python3 tools/combine_uf2.py \
    build/omega-amiga.uf2 \
    kickstart13.rom \
    combined.uf2

# Optionally add ADF floppy images
python3 tools/combine_uf2.py \
    build/omega-amiga.uf2 \
    kickstart13.rom \
    combined.uf2 \
    --adf0 workbench13.adf \
    --adf1 extras.adf
```

Hold BOOTSEL and connect USB, then copy `combined.uf2` to the `RPI-RP2` drive.

## Phase 2: Display

The `src/host.c` `display_push_frame()` stub is the only function you need
to implement. Options:

| Output | Library | Notes |
|---|---|---|
| DVI/HDMI | [PicoDVI](https://github.com/Wren6991/PicoDVI) | Best quality, needs DVI connector |
| VGA | pico-vga-scanvideo | Needs resistor ladder, 3 GPIO per channel |
| SPI TFT | st7789 / ili9341 | Easy hardware, 320×240 typical |

The framebuffer is always at `PSRAM_BASE + PSRAM_FRAMEBUF_OFFSET` in ARGB32
format, 640×400 pixels. The host presentation stage clips the raw DMA fetch
raster, applies the 27:32 Amiga HIRES-to-square-pixel aspect correction, and
selects the centred 400 lines of the emulated 480-line viewport. This behavior
is shared with the native test runner.

## Phase 2: USB HID

Wire `pressKey()`/`releaseKey()` to TinyUSB HID keyboard events.
Joystick/mouse delta goes into `chipset.joy0dat` (see original Host.c).
TinyUSB is included in the Pico SDK; add `tinyusb_host` to `target_link_libraries`.

## Architecture Notes

- **Memory.c is fully replaced** by `src/memory.c` which uses PSRAM via address
  translation (Amiga addresses → PSRAM offsets).  The original 16 MB static
  array is gone.
- **Host.c is fully replaced** by `src/host.c`.  All SDL2 code is removed.
- **Musashi 68K** (`omega/m68k*.c`) is unchanged.
- **Chipset/CIA/DMA/Blitter** (`omega/*.c`) are unchanged.
- **Floppy.c** has a `PICO_BUILD` guard: the desktop `ADF2MFM(fd,...)` still
  compiles on Linux/macOS; `ADF2MFM_from_mem(buf,size,...)` is used on RP2350.
- **CPU.c** has a `PICO_BUILD` guard in `cpu_pulse_reset()` to call
  `memory_clear_chipram()` instead of the `low16Meg` loop.
- **FatFs** is configured read-only and provides FAT16/FAT32 access for ROM
  loading. Its low-level disk layer uses the SPI1 pins in `board_config.h`.

## License

Omega is licensed under MPL 2.0.  RP2350 port additions are also MPL 2.0.
Kickstart ROMs are © Commodore/Cloanto – you must own a legal copy.
FatFs is distributed under its own permissive license in
`third_party/fatfs/LICENSE.txt`.
