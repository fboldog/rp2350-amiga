#!/usr/bin/env bash
# Deterministic framebuffer regression suite. Build and run with:
#   VIDEO=PAL ./native/regression.sh
#   VIDEO=NTSC ./native/regression.sh
# Deliberately uses exact PPM hashes: a changed pixel requires visual review
# followed by UPDATE_BASELINES=1 only when the new rendering is accepted.
set -euo pipefail

cd "$(dirname "$0")/.."

VIDEO=${VIDEO:-NTSC}
case "$VIDEO" in
    NTSC|ntsc) VIDEO_NAME=NTSC ;;
    PAL|pal)   VIDEO_NAME=PAL ;;
    *) echo "VIDEO must be NTSC or PAL" >&2; exit 2 ;;
esac

BASELINES="native/regression-baselines.sha256"
ROM_DIR="sd_card/rom"
ADF_DIR="sd_card/adf"
OUTPUT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/omega-regression-${VIDEO_NAME}.XXXXXX")
echo "regression artifacts: $OUTPUT_DIR"

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "missing test asset: $1" >&2
        exit 2
    fi
}

for asset in "$ROM_DIR/kick13.rom" "$ROM_DIR/kick204.rom" \
             "$ROM_DIR/kick205.rom" "$ROM_DIR/kick314.rom" \
             "$ADF_DIR/amiga-os-134-workbench.adf" \
             "$ADF_DIR/amiga-os-204-workbench.adf" \
             "$ADF_DIR/amiga-os-205-workbench.adf" \
             "$ADF_DIR/amiga-os-314-workbench.adf" \
             "$ADF_DIR/amiga-os-314-modules-a500.adf" \
             "$ADF_DIR/Install3.2.adf" \
             "$ADF_DIR/PrismRTG-1.2.adf"; do
    require_file "$asset"
done
for tool in rdbtool xdftool; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing $tool (amitools): needed for the hard-disk images" >&2
        exit 2
    fi
done

REGRESSION=1 VIDEO="$VIDEO_NAME" ./native/build.sh

# KickSmash ROM-switcher protocol (omega/KickSmash.c), driven as the
# switcher's transport drives it.
${CC:-cc} -O1 -Wall -Wno-unused -include native/sdl_shim.h -Isrc -Iomega \
    -o "$OUTPUT_DIR/kicksmash_test" native/kicksmash_test.c omega/KickSmash.c
"$OUTPUT_DIR/kicksmash_test"

# 68020 bitfield instructions against a bit-by-bit model.
${CC:-cc} -O1 -w -include native/sdl_shim.h -Isrc -Iomega \
    -o "$OUTPUT_DIR/bitfield_test" native/bitfield_test.c omega/m68kcpu.c \
    omega/m68kops.c omega/m68kopac.c omega/m68kopdm.c omega/m68kopnz.c \
    omega/m68kdasm.c
"$OUTPUT_DIR/bitfield_test"

RESULTS="$OUTPUT_DIR/results.sha256"
: > "$RESULTS"

run_case() {
    name=$1
    rom=$2
    adf=$3
    iterations=$4
    insert_at=$5
    hdf=${6:-}
    echo "[$VIDEO_NAME] $name"
    OMEGA_CPU="${CPU:-}" OMEGA_HDF="$hdf" OMEGA_ROM="$PWD/$rom" \
        ./native/omega-native "$adf" \
        "$iterations" 0 "$insert_at" >"$OUTPUT_DIR/$name.log" 2>&1
    cp frame_final.ppm "$OUTPUT_DIR/$name.ppm"
    checksum=$(hash_file "$OUTPUT_DIR/$name.ppm")
    printf '%s  %s/%s.ppm\n' "$checksum" "$VIDEO_NAME" "$name" >> "$RESULTS"
}

run_case kick13-insert "$ROM_DIR/kick13.rom" "" 500000 3000
run_case kick204-insert "$ROM_DIR/kick204.rom" "" 500000 3000
run_case kick314-boot "$ROM_DIR/kick314.rom" "" 500000 3000
run_case wb13 "$ROM_DIR/kick13.rom" \
    "$PWD/$ADF_DIR/amiga-os-134-workbench.adf" 500000 3000
run_case wb204 "$ROM_DIR/kick204.rom" \
    "$PWD/$ADF_DIR/amiga-os-204-workbench.adf" 500000 3000
run_case wb314 "$ROM_DIR/kick314.rom" \
    "$PWD/$ADF_DIR/Install3.2.adf" 500000 3000

# Booting from the emulated A600 IDE drive (no floppy): fresh images, as
# the boot writes to them.
./native/make_hdf.sh "$OUTPUT_DIR/hdf314.hdf" System ffs+intl \
    "$ADF_DIR/amiga-os-314-workbench.adf" \
    "$ADF_DIR/amiga-os-314-modules-a500.adf" >/dev/null
./native/make_hdf.sh "$OUTPUT_DIR/hdf205.hdf" System ffs \
    "$ADF_DIR/amiga-os-205-workbench.adf" >/dev/null
run_case hdf314 "$ROM_DIR/kick314.rom" "" 500000 3000 "$OUTPUT_DIR/hdf314.hdf"
run_case hdf205 "$ROM_DIR/kick205.rom" "" 500000 3000 "$OUTPUT_DIR/hdf205.hdf"

# The same boot on the board's 68EC020 (the KickSmash "cpu <n> 68020"
# setting): Kickstart must find no FPU and use its 68020 exec paths.
./native/make_hdf.sh "$OUTPUT_DIR/hdf314-ec020.hdf" System ffs+intl \
    "$ADF_DIR/amiga-os-314-workbench.adf" \
    "$ADF_DIR/amiga-os-314-modules-a500.adf" >/dev/null
CPU=68ec020 run_case hdf314-ec020 "$ROM_DIR/kick314.rom" "" 500000 3000 \
    "$OUTPUT_DIR/hdf314-ec020.hdf"

# RTG: the same system with PrismRTG (its PICASSO2 driver on the emulated
# Picasso II+, omega/Picasso2.c) and Workbench on PRISM:640x480 8-bit. The
# files go where its installer puts them; PrismSetup sets the Workbench mode
# (in ENVARC:, for the next boot: it is copied to ENV: for this one).
rtg_hdf="$OUTPUT_DIR/rtg314.hdf"
prism="$OUTPUT_DIR/prism"
./native/make_hdf.sh "$rtg_hdf" System ffs+intl \
    "$ADF_DIR/amiga-os-314-workbench.adf" \
    "$ADF_DIR/amiga-os-314-modules-a500.adf" >/dev/null
mkdir -p "$prism"
xdftool "$ADF_DIR/PrismRTG-1.2.adf" unpack "$prism" >/dev/null
xdftool "$rtg_hdf" open part=DH0 + write "$prism/PrismRTG/PrismD" C \
    + write "$prism/PrismRTG/PrismSetup" C + makedir Libs/Prism \
    + write "$prism/PrismRTG/Drivers/PICASSO2.driver" Libs/Prism \
    + write "$prism/PrismRTG/Prism" Devs/Monitors \
    + read S/Startup-Sequence "$OUTPUT_DIR/rtg314.ss" >/dev/null
sed -i '/^Assign >NIL: ENV: RAM:ENV$/a C:PrismSetup >NIL: WBID=0001\
C:Copy >NIL: ENVARC:Sys/screenmode.prefs ENV:Sys/' "$OUTPUT_DIR/rtg314.ss"
xdftool "$rtg_hdf" open part=DH0 + delete S/Startup-Sequence \
    + write "$OUTPUT_DIR/rtg314.ss" S/Startup-Sequence >/dev/null
CPU=68ec020 run_case rtg314 "$ROM_DIR/kick314.rom" "" 600000 3000 "$rtg_hdf"

if [ "${UPDATE_BASELINES:-0}" = "1" ]; then
    UPDATED="$OUTPUT_DIR/updated.sha256"
    echo '# Generated by UPDATE_BASELINES=1 VIDEO=<PAL|NTSC> ./native/regression.sh' > "$UPDATED"
    if [ -f "$BASELINES" ]; then
        awk -v prefix="$VIDEO_NAME/" '$1 !~ /^#/ && $2 !~ ("^" prefix)' \
            "$BASELINES" > "$OUTPUT_DIR/other.sha256"
        cat "$OUTPUT_DIR/other.sha256" "$RESULTS" | sort -k2 >> "$UPDATED"
    else
        sort -k2 "$RESULTS" >> "$UPDATED"
    fi
    cp "$UPDATED" "$BASELINES"
    echo "updated $VIDEO_NAME baselines in $BASELINES"
    exit 0
fi

EXPECTED="$OUTPUT_DIR/expected.sha256"
awk -v prefix="$VIDEO_NAME/" '$2 ~ ("^" prefix)' "$BASELINES" > "$EXPECTED"
if [ ! -s "$EXPECTED" ]; then
    echo "no $VIDEO_NAME baselines; review images in $OUTPUT_DIR, then run:" >&2
    echo "  UPDATE_BASELINES=1 VIDEO=$VIDEO_NAME ./native/regression.sh" >&2
    exit 2
fi

# Cases run in script order; compare by name.
sort -k2 -o "$EXPECTED" "$EXPECTED"
sort -k2 -o "$RESULTS" "$RESULTS"
if diff -u "$EXPECTED" "$RESULTS"; then
    echo "all $VIDEO_NAME framebuffer regressions passed"
else
    echo "framebuffer regression failed; inspect $OUTPUT_DIR" >&2
    exit 1
fi
