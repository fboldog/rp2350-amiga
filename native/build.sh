#!/usr/bin/env bash
# Build the head-less native Omega runner (no SDL, no RP2350 SDK).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=native/omega-native
CC=${CC:-gcc}

# omega/ core, minus the Musashi disassembler (not needed head-less, and it
# trips -Werror-ish format warnings under GCC 14).
OMEGA_SRC=(
  omega/Blitter.c
  omega/Chipset.c
  omega/CIA.c
  omega/CPU.c
  omega/debug.c
  omega/DMA.c
  omega/Floppy.c
  omega/Gayle.c
  omega/m68kcpu.c
  omega/m68kdasm.c
  omega/m68kopac.c
  omega/m68kopdm.c
  omega/m68kopnz.c
  omega/m68kops.c
)

set -x
$CC -O2 -g -std=gnu11 \
    -include native/sdl_shim.h \
    -Isrc -Iomega \
    -Wall -Wno-unused -Wno-implicit-fallthrough -Wno-comment -Wno-format \
    -Wno-incompatible-pointer-types \
    -o "$OUT" \
    native/main_native.c native/host_native.c native/memory_native.c \
    "${OMEGA_SRC[@]}" \
    -lm
set +x
echo "built $OUT"
