# Omega Amiga Emulator – RP2350 Bare-Metal Port

Port of [Omega](https://github.com/h5n1xp/Omega) (bare-metal 68K + Amiga chipset emulator)
to the **RP2350B** (Pimoroni Pico Plus 2), running on Cortex-M33 @ 250 MHz.

## Status – Phase 1

| Component | Status |
|---|---|
| Build system (Pico SDK 2.x) | ✅ |
| PSRAM init (APS6404L on QMI CS1) | ✅ |
| Chip RAM / Slow RAM in PSRAM | ✅ |
| ROM from flash (0x10200000) | ✅ |
| Musashi 68K CPU core | ✅ |
| Custom chipset + CIA + DMA | ✅ |
| Floppy (DF0/DF1 from flash ADF) | ✅ |
| Framebuffer in PSRAM | ✅ |
| Display output (VGA/DVI/SPI-TFT) | ⬜ Phase 2 |
| USB HID keyboard/mouse | ⬜ Phase 2 |

## Hardware

Target board: **Waveshare RP2350-PiZero** (RP2350B).  All board wiring is in
[`src/board_config.h`](src/board_config.h); the Pico SDK board header is
[`src/boards/waveshare_rp2350_pizero.h`](src/boards/waveshare_rp2350_pizero.h).

- **MCU**: RP2350B (80-pin, GPIO0..47), Cortex-M33 @ 250 MHz
- **PSRAM**: ⚠️ **not fitted from the factory** — the PiZero has an empty 8-pin
  QSPI pad (U1) on the shared QMI bus, CS = GPIO47.  Solder an APS6404L-class
  part (3.3 V quad, `0x35` enter-QPI, `0xEB` fast-read).  8 MB max on one die.
  Mapped at `0x11000000` after `psram_init()`.
- **Flash**: 16 MB QSPI (CS0) → firmware at `0x10000000`, ROM at `0x10200000`
- **UART**: TX=GPIO0, RX=GPIO1 (115200 8N1) for debug output
- **Display (Phase 2)**: on-board DVI/HDMI.  TMDS D0=GPIO36, D1=GPIO34,
  D2=GPIO32, CLK=GPIO38 (`invert_diffpairs=false`, PIO0, `gpio_base=16`) —
  i.e. PicoDVI's `pico_sock_cfg`.
- **microSD (Phase 3)**: SPI1 — SCK=GPIO30, MOSI=GPIO31, MISO=GPIO40,
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
0x11480000  DF1 MFM floppy buffer (2 MB)
0x11680000  Framebuffer 640×400 ARGB32 (1 MB)
0x11780000  Reserved (512 KB)
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
chipset frame length and video-identification bit.

Output: `build/omega-amiga.uf2`

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

## License

Omega is licensed under MPL 2.0.  RP2350 port additions are also MPL 2.0.
Kickstart ROMs are © Commodore/Cloanto – you must own a legal copy.
