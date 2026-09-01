# native/ — head-less desktop runner for the Omega core

Runs the **same `omega/*.c` sources** the RP2350 firmware uses (compiled with
`PICO_BUILD` **undefined**, so the original desktop code paths are taken), linked
against a plain `malloc`'d framebuffer and the 16 MB `low16Meg` array instead of
PSRAM. No SDL, no display — it dumps framebuffer snapshots as binary PPM.

Purpose: validate the CPU + chipset + CIA + DMA + Blitter + Floppy logic on a PC
without RP2350 hardware. It does **not** exercise `src/psram.c`, `src/Memory.c`
(PSRAM address translation), `src/main.c`, or the PSRAM framebuffer in
`src/Host.c` — those are RP2350-only.

## Build & run

```bash
./native/build.sh                       # -> native/omega-native  (needs gcc, no SDL)

# Insert-disk screen only (embedded Kickstart 1.3, no floppy):
./native/omega-native

# Boot a Workbench ADF to the AmigaDOS CLI:
./native/omega-native path/to/workbench13.adf 300000 50000 3000
#                      ^disk.adf              ^iters ^dump  ^insert-at (batch)

python3 native/ppm2png.py frame_final.ppm frame_final.png
```

Each "iteration" is 200×(`dma_execute()` + `cpu_execute()`), matching the RP2350
`main.c` main loop. `insert-at` delays the simulated DF0 disk-insert until
Kickstart has finished drive ID mode (`df[0].idMode == 0`); the runner then polls
`floppyInsert(0)` until the drive latches the disk.

## Status (2026-09-01)

- Kickstart 1.3 boots to the "insert Workbench" screen. ✅
- `original2.adf` (WB 1.3.2 UK) boots through the startup-sequence to `[CLI 2]`. ✅
- Battery-clock reads at `0xDC0000` return junk (`<invalid>` from `date`) — the
  Gayle/RTC path is a stub; unrelated to the RP2350 port.
- The ROM diskette-logo bitmap renders horizontally mirrored on the insert
  screen; normal Workbench text/graphics render correctly. Same Blitter/DMA code
  on both targets — cosmetic, low priority.

## Files

| File | Role |
|---|---|
| `main_native.c`   | entry: load ROM/ADF, run loop, dump PPM |
| `host_native.c`   | SDL-free `Host` layer; planar→chunky identical to `src/Host.c` |
| `memory_native.c` | upstream Omega `Memory.c` verbatim (`low16Meg`, `chipRead*/Write*`) |
| `sdl_shim.h`      | no-op `SDL_AtomicGet/Set` so `waitFreeSlot()` links without SDL |
| `build.sh`        | one-shot gcc build |
| `ppm2png.py`      | dependency-free P6-PPM → PNG |
