# Board tooling

Development scripts for the WeAct RP2350B board, run on the host with a
Raspberry Pi Debug Probe (CMSIS-DAP) and OpenOCD. All reads are
non-halting SWD memory reads, so HDMI scanout keeps running.

| Script | Purpose |
|---|---|
| `snap.py <elf> <256\|240> <out>` | Freeze the displayed HDMI frame (`hostCaptureHold`), dump it with its colour record and replay scanout to `<out>.png` (exact colours, sprites) |
| `rate.sh <elf>` | Emulated vblanks/s (CIA-A TOD) and shown frames/s over 10 s |
| `boot.py <elf> <256\|240> [limit]` | Time from reset to the Workbench 1.3 icon screen |
| `disk.sh <slot>` | Mount DF0 slot `<slot>` (1-based) on the next boot and reset once |

`<elf>` is the build running on the board (e.g.
`build-weact-hdmi-pal/omega-amiga.elf`); `256` rows for PAL, `240` for NTSC.
Reference screenshots for comparison are in `image_refs/`.

Notes: rapid repeated SWD resets can wedge the chip (no SWD access); recover
with `openocd -f interface/cmsis-dap.cfg -f target/rp2350-rescue.cfg -c init
-c exit` and reflash. The CIA TOD rate is meaningless while Kickstart is
booting (the OS rewrites TOD).
