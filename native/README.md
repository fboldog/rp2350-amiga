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
HEADLESS=1 ./native/build.sh            # force PPM-only build, even with SDL2
```

`build.sh` auto-detects SDL2 via `sdl2-config`:
- **SDL2 present** — a live integer-scaled 2× window opens automatically:
  1280×800 for NTSC or 1280×1024 for PAL.
  Press **Esc** or close the window to stop early; the final PPM is still written.
- **SDL2 absent** — headless PPM-only build (install `sdl2` / `libsdl2-dev` for the window).

The ROM is selected via the `OMEGA_ROM` environment variable (256 KB ROMs are
mirrored automatically to fill the 512 KB window at 0xF80000–0xFFFFFF):

```bash
# Kickstart 1.3 — insert-disk screen only (no floppy):
OMEGA_ROM=kick-13.rom ./native/omega-native "" 500000

# Kickstart 1.3 — boot a WB 1.3 ADF to the AmigaDOS CLI:
OMEGA_ROM=kick-13.rom ./native/omega-native workbench13.adf 300000 50000 3000
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

For deterministic screenshot checks, force a headless build and capture the
insert-disk screen after 500,000 iterations:

```bash
HEADLESS=1 ./native/build.sh
OMEGA_ROM=kick204.rom ./native/omega-native "" 500000 500000
python3 native/ppm2png.py frame_final.ppm frame_final.png
```

The same command works with `kick-13.rom` and `kick314.rom`. ROMs and generated
`frame*.ppm`/`frame*.png` files are local test assets and must not be committed.

### Screenshot regression suite

`REGRESSION=1` is a native compile-time build mode. It forces a headless build
and removes the large interactive boot-state diagnostics so framebuffer tests
are deterministic and concise:

```sh
REGRESSION=1 VIDEO=PAL ./native/build.sh
```

The regression runner builds in that mode, boots the three ROM-only screens and
the three Workbench/installer cases, then compares exact final-frame PPM hashes:

```sh
VIDEO=PAL  ./native/regression.sh
VIDEO=NTSC ./native/regression.sh
```

It requires the locally supplied ROM and ADF files named in the examples above.
Each run prints a unique temporary directory containing its PPMs and logs for
manual review. If an intentional rendering change has been reviewed in both
modes, update one baseline set at a time with:

```sh
UPDATE_BASELINES=1 VIDEO=PAL ./native/regression.sh
```

Never update baselines merely to make a failing test pass; inspect the generated
images first. Reference PNGs remain manual visual references because their
canvas sizes and capture sources are not uniform.

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

The video standard is selected at compile time. NTSC is the default; build PAL
with:

```sh
VIDEO=PAL ./native/build.sh
```

`VIDEO=NTSC` restores the default; lowercase `pal` and `ntsc` are also
accepted. The script is compatible with the Bash 3.2 version shipped by macOS.

The choice is shared by DMA frame length, the chipset video-identification bit,
CIA vertical TOD events, and SDL pacing (59.94 Hz NTSC or 50 Hz PAL), so
VBL-driven animations run in real time. `OMEGA_HEADLESS=1` and binaries built
with `HEADLESS=1` remain unthrottled for fast boot and framebuffer tests.

The native framebuffer follows the selected standard: 640×400 for NTSC and
640×512 for PAL. SDL creates a fixed 2× window and uses nearest-neighbour
integer scaling, so pixels are never resized by a fractional ratio. PPM dumps
use the same selected framebuffer dimensions. The RP2350 framebuffer remains
640×400 because its fixed 8 MB PSRAM map reserves 1 MB for ARGB display output.

PAL uses its own viewport origin and a full-height native intermediate raster.
The full beam-row raster is required by full-width LORES copper displays such
as the Kickstart 1.3 insert-disk requester: its logical picture rows occupy
alternating beam rows. Presentation samples those rows once and then applies
the normal 2× integer scale, avoiding both empty scanlines and truncation of the
hand and lower disk artwork. HIRES displays continue to consume half-height
logical rows. The RP2350 keeps its fixed 200-row intermediate raster because of
the existing PSRAM layout.

DMA tracks its intermediate destination with separate raster-row and X
coordinates. The planar-to-chunky helpers write relative to the destination
pointer supplied by DMA instead of sharing the old overloaded `FBCounter` for
both line and pixel offsets. Display-window vertical tests also decode the OCS
fixed ninth bits (`VSTART8=0`, `VSTOP8=1`) in one place.

Only an actual bitplane fetch selects the frame's LORES or HIRES presentation
mode. During display blanking, `BPLCON0` contains zero planes; its cleared HIRES
bit must not be interpreted as a LORES screen. Doing so made full-width HIRES
Workbench output sample alternating raster rows and appear at half height.

Legacy framebuffer-position helpers from the former `FBCounter` design have
been removed. `BPLCON0` writes no longer reset a host-side drawing cursor, and
the unused `setDisplayMode()` plus random-pixel `drawBlank()` diagnostic are
gone. Raster placement is now owned solely by the active DMA fetch paths.

Planar-to-chunky conversion APIs take only their destination and bitplane
words. The former palette argument was unused because conversion reads the
live chipset palette, so removing it keeps the interface consistent with the
actual data dependency.

HIRES, LORES, and HAM planar conversion now live in the shared
`src/Planar.c`. Native and RP2350 builds link the same implementation instead
of carrying byte-for-byte copies in their host backends.

Framebuffer presentation now lives in shared `src/Presentation.c`. Clipping,
LORES row selection, scanline expansion, wrapped-prefix repair, aspect mapping,
and raster clearing therefore have one implementation for native and RP2350
builds. Each host backend retains only platform-specific frame submission and
lifecycle work.

The shared presentation code names its display-layout signatures, row
rotation, wrapped-prefix width, and OCS vertical-bank size rather than
scattering literal register and pixel values through the algorithm. Repeated
full-width and wrapped-window predicates are expressed as helpers, including
removal of a redundant wrapped-origin test.

DDF layout signatures shared by DMA and presentation are defined in
`omega/DisplayLayout.h`. Full-width fetch detection, the extra-word requester
layout, and the rotated Workbench 3.14 layout can no longer drift between the
two stages.

The same layout header also names the normal and extra-word HIRES fetch tails,
the LORES fetch span, and the two upper-overscan offsets. DMA no longer embeds
those timing and placement values directly in its control flow.

Bitplane DMA now has an explicit per-scanline state object for the terminal
fetch cycle and HIRES/LORES word counts. This replaces unrelated loose globals
and provides one owner for the fetch-completion and modulo timing model. The
current modulo behavior is intentionally unchanged at this foundation step.

Scanline reset and bitplane-pointer advancement are now explicit operations on
that boundary, and the state records whether a real plane fetch occurred. For
compatibility, modulo is still applied at the existing host scanline boundary;
the fetch marker allows no-fetch advancement to be measured before replacing
it with hardware DDF-completion timing.

Fetch tracking covers every enabled plane, not only plane 1. This captures
partial Copper transition lines where higher planes fetch before `BPLCON0`
blanks the display. Fully blank lines still use compatibility modulo
advancement because removing it currently exposes incomplete Copper pointer
reload behavior in the 2.04 and 3.14 layouts.

The scanline state stores a per-plane fetch mask rather than a single boolean.
Partial Copper transition lines can therefore distinguish odd-plane and
even-plane activity, which is necessary before their two modulo registers can
be timed independently. It also retains the union of planes enabled at any
point during the line, separately from planes that actually fetched, so
Copper mode changes are not reduced to the final `BPLCON0` value. A per-plane
fetch-eligible mask is accumulated while bitplane DMA and the display window
are both active, distinct from both merely enabled planes and planes for which
a DMA word was observed. Real fetch marking is shared by all implemented OCS
plane paths. Obsolete per-plane/group cycle diagnostics and the redundant
completed-fetch copy were removed once DDF completion could consume the live
fetch mask directly. On an otherwise eligible line
where no fetch occurs, fallback advancement is limited to that eligible set;
fully ineligible lines reuse the most recently fetched plane set. The legacy
all-plane startup fallback has been removed, so no pointer receives speculative
modulo advancement before the first observed bitplane fetch.

The `BPLCON0` decoder caps its plane count to the implemented OCS DMA
capacity—four planes in HIRES and six in LORES—so every consumer sees one
supported mask. Fetch eligibility uses that decoded mask directly instead of
independently interpreting raw register bits.

The final DDF cycle explicitly snapshots the completed per-plane fetch mask.
This provides a stable completion hook for relocating modulo timing later,
without yet changing the scanline-boundary application used by rendering. The
corresponding HIRES pointers for planes 1–4 are captured at the same boundary,
separating the fetched line from any later Copper pointer writes. The
narrow-DDF right-edge prefetch now reads these captured pointers instead of
live end-of-line values, preventing late Copper writes from changing a line
whose fetch window already completed. Modulo for planes that really fetched is
now applied exclusively at this DDF-completion boundary. The former
end-of-line safety application and its double-advance mask have been removed;
no-fetch compatibility advancement is the only modulo operation that remains
at the scanline boundary.

Bitplane pointer register writes are tracked per plane and per scanline. Both
CPU and Copper writes pass through the same register handlers, allowing DMA to
correlate pointer reloads with the planes that fetched before blank-line
compatibility advancement is removed. High- and low-word writes are recorded
separately, so a complete pointer reload can be distinguished from an update
to only half of a pointer. A per-plane reload mask records when both words
were written during the same scanline; this remains observational until it is
used to narrow the blank-line compatibility path. The horizontal beam cycle
of the first complete reload is also retained, allowing later logic to relate
the reload to the DDF fetch window instead of treating every reload equally.
Each completion is classified as occurring before, during, or after the active
DDF interval for its scanline. Cross-scanline reload diagnostics were tested
while investigating the no-fetch fallback, then removed after they proved the
fallback dependency was unrelated to pending complete reloads.

Partial scanlines now apply `BPL1MOD` and `BPL2MOD` only to odd/even plane
members that actually fetched, so an unfetched sibling pointer no longer
advances merely because another plane in its modulo group was active. Fully
blank lines retain compatibility
advancement at the scanline boundary, except when every active plane in a
group completed a pointer reload before DDF. Requiring the full active group
avoids treating a partial Copper pointer update as a reload of all odd or even
planes. The active set is accumulated across the full scanline rather than
taken from the final `BPLCON0` value, preserving modes that the Copper changes
before the line ends. Because pointer advancement is now per plane, a
pre-fetch reload suppresses fallback advancement only for that active plane;
unrelated pointers in the same modulo group are unaffected. Applying modulo
at the emulated terminal fetch slot was tested and rejected for now because it
shifted the extra right-edge fetch used by the Kickstart 2.04 requester.
Modulo policy selection and pointer mutation are now separate internal steps,
so a future timing change can relocate application without duplicating the
per-plane and compatibility rules.

The shared layout definitions also name the raster origin at beam line 43 and
the first LORES render line at 44. LORES and HIRES positioning now refer to
those meanings rather than repeating unexplained vertical literals.

The unused `sprite2chunky()` API and its duplicate host implementations have
been removed. This does not remove working sprite support: `spriteCycle()` is
currently an explicit stub, so sprite DMA and sprite rendering remain
unimplemented. Implementing the sprite fetch/state machine is still required
before a shared sprite converter should be introduced again.

Always-on rendering investigation logs have also been removed from the DMA and
custom-register hot paths. Per-line HIRES, `BPLCON0`, and modulo writes no
longer add console I/O or timing noise during normal emulation; screenshot
regression logs retain the runner-level information needed for failures.

## Status (2026-09-05)

- Kickstart 1.3 boots to the complete "insert Workbench" screen. ✅
  The native host records the full PAL/NTSC beam field and tracks whether the
  active display is LORES or HIRES. Full-width LORES output is deinterlaced
  before 2× presentation, so the requester has continuous scanlines, correct
  vertical scale, and its complete lower half.
- Loaded Workbench screens retain their full vertical height. ✅
  Zero-plane blanking no longer changes the remembered display resolution, so
  HIRES frames are not mistakenly passed through the LORES alternating-row
  presentation path. This applies to Workbench 1.3, 2.04, and 3.14 while
  leaving their insert-disk requester output unchanged.
- `original2.adf` (WB 1.3.2 UK) boots through the startup-sequence to `[CLI 2]`. ✅
  Full-width HIRES Workbench screens use a true 640-pixel raster stride rather
  than the narrower wrapped-fetch presentation used by the Kickstart artwork.
  PAL Copper waits that cross line 255 retain the next vertical-line bank, and
  the 40-line upper overscan is removed before presentation. This keeps the
  complete loading window—including its lower border—in the 640×400 output.
- Kickstart 2.04 boots Workbench 2.x from ADF (`Install3.2.adf` confirmed). ✅
  Its `DDFSTRT=0x3c`, `DDFSTOP=0xd4` HIRES window consumes 40 words per
  bitplane row; the extra fetch needed by the narrower 1.3 window is applied
  only to `0x3c–0xd0`. This prevents the row-by-row pointer drift that produced
  diagonally offset blocks. The desktop is coherent, although its final PAL
  viewport framing still needs refinement.
  Byte reads outside the 32 readable custom-register byte handlers return open
  bus instead of indexing beyond the dispatch table; this is shared by the
  native and RP2350 memory backends.
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
  visible scanlines. It then maps the Amiga HIRES pixel aspect at 27:32 and
  selects the centred 400-line region of the 480-line viewport. Normal and
  vertically wrapped display windows use their appropriate horizontal origins.
  This presentation is shared by Kickstart 1.3, 2.04, and 3.14.
- Kickstart 3.14 boots ROM-based Workbench (grey backdrop + title bar). ✅
  (Regression after CIA ICR fix was resolved by implementing the chipset
  slow-RAM mirror: reads at `0xCxxxxxx & 0xFFF < 0x20` now route to the
  correct chipset read-register instead of raw chip RAM.) Its full-width
  `DDFSTRT=0x38`, `DDFSTOP=0xd8` desktop raster requires an 80-pixel circular
  presentation correction; this keeps the rightmost segment on the right
  instead of wrapping it to the left edge. The correction is shared by the
  native and RP2350 hosts and does not affect the 1.3 or 2.04 desktop modes.
- Battery-clock reads at `0xDC0000` return junk (`<invalid>` from `date`) — the
  Gayle/RTC path is a stub; unrelated to the RP2350 port.
- The ROM diskette-logo bitmap renders horizontally mirrored on the insert
  screen; normal Workbench text/graphics render correctly. Same Blitter/DMA code
  on both targets — cosmetic, low priority.

## Files

| File | Role |
|---|---|
| `main_native.c`   | entry: load ROM/ADF, run loop, dump PPM |
| `host_native.c`   | native framebuffer lifecycle, PPM inspection, and SDL frame submission |
| `../src/Planar.c` | shared HIRES, LORES, and HAM planar-to-chunky conversion |
| `../src/Presentation.c` | shared raster clipping, repair, scaling, and framebuffer presentation |
| `memory_native.c` | upstream Omega `Memory.c` verbatim (`low16Meg`, `chipRead*/Write*`) |
| `display_sdl.c`   | SDL2 window: open / push (30 fps cap) / poll / close |
| `display_sdl.h`   | public API for `display_sdl.c` |
| `sdl_shim.h`      | no-op `SDL_AtomicGet/Set` so `waitFreeSlot()` links without SDL |
| `build.sh`        | gcc build; auto-detects SDL2, compiles `display_sdl.c` without the shim |
| `ppm2png.py`      | dependency-free P6-PPM → PNG |
