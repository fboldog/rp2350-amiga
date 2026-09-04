# native/ — head-less desktop runner for the Omega core

Runs the **same `omega/*.c` sources** the RP2350 firmware uses (compiled with
`PICO_BUILD` **undefined**, so the original desktop code paths are taken), linked
against a plain `malloc`'d framebuffer and the 16 MB `low16Meg` array instead of
PSRAM. Displays in a live SDL2 window when SDL2 is available; otherwise dumps
framebuffer snapshots as binary PPM.

Purpose: validate the CPU + chipset + CIA + DMA + Blitter + Floppy logic on a PC
without RP2350 hardware. It does **not** exercise `src/psram.c`, `src/Memory.c`
(PSRAM address translation), `src/main.c`, or the PSRAM framebuffer in
`src/Host.c` — those are RP2350-only.

## Build & run

```bash
./native/build.sh                       # -> native/omega-native
```

`build.sh` auto-detects SDL2 via `sdl2-config`:
- **SDL2 present** — a live 1280×800 window (2× nearest-neighbour) opens automatically.
  Press **Esc** or close the window to stop early; the final PPM is still written.
- **SDL2 absent** — headless PPM-only build (install `sdl2` / `libsdl2-dev` for the window).

The ROM is selected via the `OMEGA_ROM` environment variable (256 KB ROMs are
mirrored automatically to fill the 512 KB window at 0xF80000–0xFFFFFF):

```bash
# Kickstart 1.3 — insert-disk screen only (no floppy):
./native/omega-native

# Kickstart 1.3 — boot a WB 1.3 ADF to the AmigaDOS CLI:
OMEGA_ROM=kick13.rom ./native/omega-native workbench13.adf 300000 50000 3000
#                                           ^disk.adf       ^iters ^dump ^insert-at

# Kickstart 2.04 — ROM disk, no floppy (purple "Insert Workbench" backdrop):
OMEGA_ROM=kick204.rom ./native/omega-native "" 500000

# Kickstart 2.04 — boot a WB 2.x / Install ADF:
OMEGA_ROM=kick204.rom ./native/omega-native Install3.2.adf 500000 4000 3000

# Kickstart 3.14 — ROM-based Workbench, no floppy (boots automatically):
OMEGA_ROM=kick314.rom ./native/omega-native "" 500000

# Convert the final PPM snapshot to PNG:
python3 native/ppm2png.py frame_final.ppm frame_final.png
```

Each "iteration" is 200×(`dma_execute()` + `cpu_execute()`), matching the RP2350
`main.c` main loop. Rough timing guide:

| Iterations | Simulated VBLs | Wall time (PC) | Notes |
|---|---|---|---|
| 40 000  |   133 |  ~1 s | Initial screen up (KS 1.3 / 2.04 backdrop) |
| 500 000 | 1 667 |  ~8 s | KS 2.04 + ADF boots to Workbench desktop |
| 500 000 | 1 667 | ~8 s  | KS 3.14 ROM-Workbench fully rendered |

`insert-at` delays the simulated DF0 disk-insert until Kickstart has finished
drive ID mode (`df[0].idMode == 0`); the runner then polls `floppyInsert(0)`
until the drive latches the disk. Use `""` as the disk argument to run without
any floppy image.

## Status (2026-09-03)

- Kickstart 1.3 boots to the "insert Workbench" screen. ✅
- `original2.adf` (WB 1.3.2 UK) boots through the startup-sequence to `[CLI 2]`. ✅
- Kickstart 2.04 boots Workbench 2.x from ADF (`Install3.2.adf` confirmed). ✅
  Title bar, Ram Disk volume, and disk name all render correctly (~1000 VBLs).
- Kickstart 2.04 no-disk boot: insert-disk screen renders correctly. ✅
  Rainbow V-checkmark, both floppy-disk icons, and all four copyright-text lines
  are visible. Fixed by: (a) floppy drive ID/motor-on reports `/DKRDY=0` for
  drive 0 regardless of ADF; (b) no-disk MFM tracks pre-filled with sync word
  so `DSKBLK` fires and trackdisk returns `TDERR_NoSecHdr`; (c) HIRES bitplane
  fetch count corrected to the real OCS formula (`lastFetchCycle = ddfstop + 7`,
  giving `(ddfstop − ddfstrt)/4 + 2` words/line); (d) `BPL1MOD` sign-extended
  correctly (`(int16_t)value >> 1`).
  The native/RP2350 host presentation step now separates the raw DMA raster
  from the displayed framebuffer, clips fetch-pipeline overscan, and doubles
  visible scanlines into the 640×400 output. This removes the left-edge disk
  ghost and the black lower half formerly caused by exposing the raw raster.
- Kickstart 3.14 boots ROM-based Workbench (grey backdrop + title bar). ✅
  (Regression after CIA ICR fix was resolved by implementing the chipset
  slow-RAM mirror: reads at `0xCxxxxxx & 0xFFF < 0x20` now route to the
  correct chipset read-register instead of raw chip RAM.)
- Battery-clock reads at `0xDC0000` return junk (`<invalid>` from `date`) — the
  Gayle/RTC path is a stub; unrelated to the RP2350 port.
- The ROM diskette-logo bitmap renders horizontally mirrored on the insert
  screen; normal Workbench text/graphics render correctly. Same Blitter/DMA code
  on both targets — cosmetic, low priority.

## Files

| File | Role |
|---|---|
| `main_native.c`   | entry: load ROM/ADF, run loop, dump PPM |
| `host_native.c`   | `Host` layer; planar→chunky identical to `src/Host.c`; calls `sdl_display_push` each VBL |
| `memory_native.c` | upstream Omega `Memory.c` verbatim (`low16Meg`, `chipRead*/Write*`) |
| `display_sdl.c`   | SDL2 window: open / push (30 fps cap) / poll / close |
| `display_sdl.h`   | public API for `display_sdl.c` |
| `sdl_shim.h`      | no-op `SDL_AtomicGet/Set` so `waitFreeSlot()` links without SDL |
| `build.sh`        | gcc build; auto-detects SDL2, compiles `display_sdl.c` without the shim |
| `ppm2png.py`      | dependency-free P6-PPM → PNG |
