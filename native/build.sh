#!/usr/bin/env bash
# Build the native Omega runner.
#
# SDL2 window support:
#   auto (default)  — enabled if sdl2-config is found, else headless PPM-only
#   HEADLESS=1      — force headless even when SDL2 is present (e.g. for CI/testing)
#
# Runtime override (SDL2 binary only):
#   OMEGA_HEADLESS=1  — suppress the window without recompiling
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=native/omega-native
CC=${CC:-gcc}

SDL_FLAGS=""
SDL_LIBS=""
SDL_SRC=""
if [ "${HEADLESS:-0}" = "1" ]; then
    echo "HEADLESS=1 — building without SDL2"
elif command -v sdl2-config >/dev/null 2>&1; then
    SDL_FLAGS="-DHAVE_SDL2 $(sdl2-config --cflags)"
    SDL_LIBS="$(sdl2-config --libs)"
    SDL_SRC="native/display_sdl.c"
    echo "SDL2 detected — building with windowed output  (HEADLESS=1 to suppress)"
else
    echo "SDL2 not found — building headless  (install sdl2 / libsdl2-dev for a window)"
fi

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

SDL_OBJ=""
if [ -n "$SDL_SRC" ]; then
    # display_sdl.c includes SDL2/SDL.h directly.  Compiling it without
    # -include sdl_shim.h avoids a conflict between the shim's static-inline
    # SDL_AtomicGet stub and SDL2's extern declaration of the same function.
    # shellcheck disable=SC2086
    $CC -O2 -g -std=gnu11 \
        -Isrc -Iomega \
        -Wall -Wno-unused -Wno-implicit-fallthrough -Wno-comment -Wno-format \
        -Wno-incompatible-pointer-types \
        $SDL_FLAGS \
        -c -o native/display_sdl.o native/display_sdl.c
    SDL_OBJ="native/display_sdl.o"
fi

# shellcheck disable=SC2086
$CC -O2 -g -std=gnu11 \
    -include native/sdl_shim.h \
    -Isrc -Iomega \
    -Wall -Wno-unused -Wno-implicit-fallthrough -Wno-comment -Wno-format \
    -Wno-incompatible-pointer-types \
    $SDL_FLAGS \
    -o "$OUT" \
    native/main_native.c native/host_native.c native/memory_native.c \
    $SDL_OBJ \
    "${OMEGA_SRC[@]}" \
    $SDL_LIBS -lm
set +x
echo "built $OUT"
