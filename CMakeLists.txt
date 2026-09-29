cmake_minimum_required(VERSION 3.18)

# ── Board / SDK setup ────────────────────────────────────────────────────
# An existing Pico SDK can be selected with PICO_SDK_PATH. If it is omitted,
# pico_sdk_import.cmake downloads the pinned SDK release into the build tree.
#
# Target board: WeAct Studio RP2350B Core (populated PSRAM on GPIO0, HDMI on
# the HSTX pins GPIO12..19). Its board header ships with Pico SDK 2.x.
set(PICO_BOARD weact_studio_rp2350b_core CACHE STRING "Target board")
if(NOT PICO_BOARD STREQUAL "weact_studio_rp2350b_core")
    message(FATAL_ERROR "Only PICO_BOARD=weact_studio_rp2350b_core is supported")
endif()

option(OMEGA_ENABLE_HDMI "Build the HSTX HDMI output" OFF)
option(OMEGA_ENABLE_SDCARD "Build microSD/FatFs Kickstart loading" OFF)
option(OMEGA_ENABLE_FLASH_FLOPPY "Load ADF images embedded in flash" OFF)
option(OMEGA_DF0_INSERT_AT_BOOT "Start flash-backed DF0 inserted at power-on" OFF)
# HDMI builds only: HSTX runs from PLL_USB, so clk_sys is independent of the
# video mode. PSRAM runs at clk_sys / ceil(clk_sys / 133 MHz).
# 320 MHz (PSRAM at 107 MHz) boots fastest; 266 MHz gives PSRAM 133 MHz; a
# multiple of 48 MHz (e.g. 240) would let USB run from PLL_SYS.
set(OMEGA_SYS_CLK_KHZ 320000 CACHE STRING "clk_sys in kHz for HDMI builds")

# Match the small hardware_psram test project: describe the physical PSRAM to
# the SDK and let its startup code configure QMI before main().
set(PSRAM_CS_PIN 0 CACHE STRING "PSRAM chip-select GPIO")

set(PSRAM_SIZE_BYTES 8388608 CACHE STRING "PSRAM capacity in bytes")
set(PICO_PSRAM_SIZE_BYTES ${PSRAM_SIZE_BYTES} CACHE STRING
    "Pico SDK PSRAM linker region size")


# Pull in Pico SDK (must be before project())
include(pico_sdk_import.cmake)
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

# ── Sources ───────────────────────────────────────────────────────────────
# RP2350-specific files (override platform-dependent Omega originals)
set(PLATFORM_SOURCES
    src/main.c
    src/psram.c
    src/Memory.c
    src/Host.c
    src/Planar.c
    src/Presentation.c
)

if(OMEGA_ENABLE_HDMI)
    list(APPEND PLATFORM_SOURCES src/dvi_display.c)
endif()

if(OMEGA_ENABLE_SDCARD)
    list(APPEND PLATFORM_SOURCES
        src/sd_card.c
        src/sd_diskio.c
        third_party/fatfs/ff.c
    )
endif()

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
    PICO_PSRAM_CS_PIN=${PSRAM_CS_PIN}
    PICO_PSRAM_SIZE_BYTES=${PSRAM_SIZE_BYTES}
    PICO_AUTO_DETECT_PSRAM_SIZE=1
    OMEGA_ENABLE_HDMI=$<BOOL:${OMEGA_ENABLE_HDMI}>
    OMEGA_ENABLE_SDCARD=$<BOOL:${OMEGA_ENABLE_SDCARD}>
    OMEGA_ENABLE_FLASH_FLOPPY=$<BOOL:${OMEGA_ENABLE_FLASH_FLOPPY}>
    OMEGA_DF0_INSERT_AT_BOOT=$<BOOL:${OMEGA_DF0_INSERT_AT_BOOT}>
    OMEGA_VIDEO_STANDARD=${OMEGA_VIDEO_STANDARD_VALUE}
    OMEGA_SYS_CLK_KHZ=${OMEGA_SYS_CLK_KHZ}u
    # Nothing on the device mallocs; keep the SDK's minimum heap so the SRAM
    # goes to the frames and the core-1 ring.
    PICO_HEAP_SIZE=0x100
)


# ── Pico SDK libraries ────────────────────────────────────────────────────
target_link_libraries(omega-amiga
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_psram
)

if(OMEGA_ENABLE_HDMI)
    target_link_libraries(omega-amiga pico_multicore hardware_vreg
                          hardware_irq hardware_resets hardware_pll)
endif()

if(OMEGA_ENABLE_SDCARD)
    target_link_libraries(omega-amiga hardware_spi)
endif()

# ── Board-specific stdio ──────────────────────────────────────────────────
pico_enable_stdio_uart(omega-amiga 1)
pico_enable_stdio_usb(omega-amiga 0)
set(OMEGA_STDIO_DESCRIPTION "UART1 GPIO4/5")

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


message(STATUS "Board:       ${PICO_BOARD}")
message(STATUS "SDK path:    ${PICO_SDK_PATH}")
message(STATUS "Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "Video mode:  ${OMEGA_VIDEO_MODE}")
message(STATUS "Sys clock:   ${OMEGA_SYS_CLK_KHZ} kHz (HDMI builds)")
message(STATUS "PSRAM:      ${PSRAM_SIZE_BYTES} bytes, CS GPIO${PSRAM_CS_PIN}")
message(STATUS "HDMI:       ${OMEGA_ENABLE_HDMI}")
message(STATUS "SD card:    ${OMEGA_ENABLE_SDCARD}")
message(STATUS "Flash ADF:  ${OMEGA_ENABLE_FLASH_FLOPPY}")
message(STATUS "DF0 at boot:${OMEGA_DF0_INSERT_AT_BOOT}")
message(STATUS "Console:    ${OMEGA_STDIO_DESCRIPTION}")
