cmake_minimum_required(VERSION 3.18)

# ── Board / SDK setup ────────────────────────────────────────────────────
# Point PICO_SDK_PATH to your Pico SDK 2.x checkout before running cmake:
#   export PICO_SDK_PATH=/path/to/pico-sdk
# Or set -DPICO_SDK_PATH=... on the cmake command line.
#
# Target board: Waveshare RP2350-PiZero (RP2350B, 16MB flash, PSRAM pad on
# GPIO47, DVI, microSD).  We ship a local Pico SDK board header for it since
# SDK 2.1.1 does not include one.  All peripheral pins live in
# src/board_config.h; psram_init() in src/psram.c does the QMI CS1 setup.
#
# To build for the original Pimoroni Pico Plus 2 instead, set
#   -DPICO_BOARD=pimoroni_pico_plus2_rp2350
# and define BOARD_* overrides, or revert to -DPICO_BOARD=pico2 (RP2350A).
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/src/boards)
set(PICO_BOARD waveshare_rp2350_pizero CACHE STRING "Target board")

# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)

include(FetchContent)
FetchContent_Declare(picodvi
    GIT_REPOSITORY https://github.com/Wren6991/PicoDVI.git
    GIT_TAG dccd738bfa9af75badcb32acde3e41bd6a3fa30a
    GIT_SHALLOW TRUE
    PATCH_COMMAND git apply --whitespace=nowarn
                  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picodvi-rp2350b.patch
)

project(omega-amiga C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(OMEGA_VIDEO_MODE "NTSC" CACHE STRING "Amiga video standard: NTSC or PAL")
set_property(CACHE OMEGA_VIDEO_MODE PROPERTY STRINGS NTSC PAL)
string(TOUPPER "${OMEGA_VIDEO_MODE}" OMEGA_VIDEO_MODE)
if(OMEGA_VIDEO_MODE STREQUAL "NTSC")
    set(OMEGA_VIDEO_STANDARD_VALUE 0)
elseif(OMEGA_VIDEO_MODE STREQUAL "PAL")
    set(OMEGA_VIDEO_STANDARD_VALUE 1)
else()
    message(FATAL_ERROR "OMEGA_VIDEO_MODE must be NTSC or PAL")
endif()

pico_sdk_init()

# Import only PicoDVI's driver library, not its examples or top-level project.
FetchContent_GetProperties(picodvi)
if(NOT picodvi_POPULATED)
    FetchContent_Populate(picodvi)
endif()
add_subdirectory(${picodvi_SOURCE_DIR}/software/libdvi
                 ${picodvi_BINARY_DIR}/libdvi)

# ── Sources ───────────────────────────────────────────────────────────────
# RP2350-specific files (override platform-dependent Omega originals)
set(PLATFORM_SOURCES
    src/main.c
    src/psram.c
    src/Memory.c
    src/Host.c
    src/Planar.c
    src/Presentation.c
    src/dvi_display.c
    src/sd_card.c
    src/sd_diskio.c
    third_party/fatfs/ff.c
)

# Omega emulator core (platform-independent)
set(OMEGA_SOURCES
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

add_executable(omega-amiga
    ${PLATFORM_SOURCES}
    ${OMEGA_SOURCES}
)

# ── Include paths ─────────────────────────────────────────────────────────
# src/ comes first so our Memory.h / host.h shadow the SDL originals in omega/
target_include_directories(omega-amiga PRIVATE
    src
    omega
    third_party/fatfs
)

# ── Compile definitions ────────────────────────────────────────────────────
target_compile_definitions(omega-amiga PRIVATE
    PICO_BUILD=1
    PICO_PSRAM_CS_PIN=47
    PICO_AUTO_DETECT_PSRAM_SIZE=1
    PICO_DEFAULT_PSRAM_MAX_FREQ=50000000
    PICO_DEFAULT_PSRAM_MIN_DESELECT=50
    # Overclock: vreg / PLL config in main.c sets the actual clock;
    # this constant is just for documentation.
    OMEGA_SYS_KHZ=250000
    OMEGA_VIDEO_STANDARD=${OMEGA_VIDEO_STANDARD_VALUE}
)

# ── Pico SDK libraries ────────────────────────────────────────────────────
target_link_libraries(omega-amiga
    pico_stdlib
    pico_multicore
    hardware_vreg
    hardware_clocks
    hardware_dma
    hardware_spi
    hardware_psram
    hardware_structs    # qmi_hw
    hardware_xip_cache  # xip_cache_invalidate_all() (RP2350 XIP maintenance)
    libdvi
    # hardware_dma      # uncomment if you add DMA transfers for display
)

# ── UART stdio (printf → UART0) ───────────────────────────────────────────
pico_enable_stdio_uart(omega-amiga 1)
pico_enable_stdio_usb(omega-amiga 0)

# ── Output formats ────────────────────────────────────────────────────────
pico_add_extra_outputs(omega-amiga)   # generates .uf2, .hex, .bin, .map

# ── ROM embedding ─────────────────────────────────────────────────────────
# The Kickstart ROM must be placed at flash offset 0x200000.
# After building, combine firmware.uf2 + kickstart.rom into a single image:
#
#   python3 tools/combine_uf2.py omega-amiga.uf2 kickstart.rom combined.uf2
#
# The combine_uf2.py script handles the flash offset and UF2 block IDs.
# See tools/combine_uf2.py for details.
#
# Alternatively: use picotool to load both separately:
#   picotool load omega-amiga.uf2
#   picotool load kickstart.rom -o 0x10200000

# ── Compiler flags ────────────────────────────────────────────────────────
# -O2 is the SDK default; push to -O3 for the emulator inner loop.
target_compile_options(omega-amiga PRIVATE
    -O3
    -Wall
    -Wno-unused-function
    -Wno-unused-variable
    # Musashi generates lots of implicit-fallthrough; suppress it
    -Wno-implicit-fallthrough
)

target_compile_definitions(omega-amiga PRIVATE
    DVI_VERTICAL_REPEAT=2
    # Two 720-pixel TMDS buffers fit beside Musashi's large dispatch tables
    # in the RP2350's 520 KB SRAM. DMA consumes one while core 1 fills the other.
    DVI_N_TMDS_BUFFERS=2
    PICO_PIO_USE_GPIO_BASE=1
)

message(STATUS "Board:       ${PICO_BOARD}")
message(STATUS "SDK path:    $ENV{PICO_SDK_PATH}")
message(STATUS "Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "Video mode:  ${OMEGA_VIDEO_MODE}")
