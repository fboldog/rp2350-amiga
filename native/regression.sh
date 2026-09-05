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

for asset in kick-13.rom kick204.rom kick314.rom \
             amiga-os-134-workbench.adf amiga-os-204-workbench.adf \
             Install3.2.adf; do
    require_file "$asset"
done

REGRESSION=1 VIDEO="$VIDEO_NAME" ./native/build.sh

RESULTS="$OUTPUT_DIR/results.sha256"
: > "$RESULTS"

run_case() {
    name=$1
    rom=$2
    adf=$3
    iterations=$4
    insert_at=$5
    echo "[$VIDEO_NAME] $name"
    OMEGA_ROM="$PWD/$rom" ./native/omega-native "$adf" \
        "$iterations" 0 "$insert_at" >"$OUTPUT_DIR/$name.log" 2>&1
    cp frame_final.ppm "$OUTPUT_DIR/$name.ppm"
    checksum=$(hash_file "$OUTPUT_DIR/$name.ppm")
    printf '%s  %s/%s.ppm\n' "$checksum" "$VIDEO_NAME" "$name" >> "$RESULTS"
}

run_case kick13-insert kick-13.rom "" 500000 3000
run_case kick204-insert kick204.rom "" 500000 3000
run_case kick314-boot kick314.rom "" 500000 3000
run_case wb13 kick-13.rom "$PWD/amiga-os-134-workbench.adf" 500000 3000
run_case wb204 kick204.rom "$PWD/amiga-os-204-workbench.adf" 500000 3000
run_case wb314 kick314.rom "$PWD/Install3.2.adf" 500000 3000

if [ "${UPDATE_BASELINES:-0}" = "1" ]; then
    if [ -f "$BASELINES" ]; then
        awk -v prefix="$VIDEO_NAME/" '$2 !~ ("^" prefix)' "$BASELINES" > "$OUTPUT_DIR/other.sha256"
        cat "$OUTPUT_DIR/other.sha256" "$RESULTS" | sort -k2 > "$BASELINES"
    else
        sort -k2 "$RESULTS" > "$BASELINES"
    fi
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

if diff -u "$EXPECTED" "$RESULTS"; then
    echo "all $VIDEO_NAME framebuffer regressions passed"
else
    echo "framebuffer regression failed; inspect $OUTPUT_DIR" >&2
    exit 1
fi
