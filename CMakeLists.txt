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
option(OMEGA_ENABLE_SDCARD "Build microSD: Kickstart from the card, IDE hard disk" OFF)
set(OMEGA_SD_ROM "rom/kick13.rom" CACHE STRING "Kickstart file on the SD card's FAT partition")
option(OMEGA_ENABLE_FLASH_FLOPPY "Load ADF images embedded in flash" OFF)
option(OMEGA_DF0_INSERT_AT_BOOT "Start flash-backed DF0 inserted at power-on" OFF)
# Zorro II boards in the autoconfig chain: 2 MB of Fast RAM, then the RTG
# card (a Picasso II+ with 1 MB of VRAM, shown on HDMI; it needs the Fast
# RAM, which puts its VRAM at 0x400000). OFF leaves the board out and its
# SRAM code too: the core-1 ring gets the room back.
option(OMEGA_FAST_RAM "Zorro II Fast RAM board (2 MB, PSRAM)" ON)
option(OMEGA_RTG "Picasso II+ RTG card (needs OMEGA_FAST_RAM and HDMI)" ON)
# The CPU of a Kickstart from flash and of KickSmash banks without a
# "cpu <n> ..." line in /rom/kicksmash.cfg: 68000 (A500) or 68020 (a
# 68EC020, which RTG drivers need).
set(OMEGA_CPU "68000" CACHE STRING "Default CPU: 68000 or 68020")
set_property(CACHE OMEGA_CPU PROPERTY STRINGS 68000 68020)
if(NOT OMEGA_CPU MATCHES "^680[02]0$")
    message(FATAL_ERROR "OMEGA_CPU must be 68000 or 68020")
endif()
# HDMI builds only: HSTX runs from PLL_USB, so clk_sys is independent of the
# video mode. PSRAM runs at clk_sys / ceil(clk_sys / 133 MHz).
# USB host needs 48 MHz from PLL_SYS (PLL_USB drives HSTX), so clk_sys must
# be a multiple of 48 MHz: 336 MHz (PSRAM and flash at 112 MHz); 320 MHz
# (without USB host) was the previous default.
set(OMEGA_SYS_CLK_KHZ 336000 CACHE STRING "clk_sys in kHz for HDMI builds")
option(OMEGA_REALTIME
       "Limit emulation to real Amiga speed (frame pacing on core 0)" ON)
option(OMEGA_ENABLE_USB_HOST
       "USB host on the native port for HID mice and keyboards (HDMI builds)" ON)
# 68000 cycles per DMA slot (colour clock): 2 matches an A500 (7.09 MHz CPU,
# 3.55 MHz colour clock); Omega used 16 (~8x fast against the beam).
set(OMEGA_CPU_CYCLES_PER_SLOT 2 CACHE STRING "68000 cycles per DMA slot (A500: 2)")
if(NOT OMEGA_CPU_CYCLES_PER_SLOT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "OMEGA_CPU_CYCLES_PER_SLOT must be a positive integer")
endif()
# DMA slots per 68000 slice: longer slices cut emulator overhead, shorter
# ones let the CPU see the beam in finer steps.
set(OMEGA_CPU_SLICE_SLOTS 8 CACHE STRING "DMA slots per 68000 execution slice")
option(OMEGA_SLOT_SKIP "Skip runs of idle DMA slots in dma_run()" ON)
option(OMEGA_HOT_CODE_IN_RAM
       "Run the emulator's hottest files (DMA, CIA, blitter, floppy, CPU loop, memory, host) from SRAM" ON)
option(OMEGA_HOT_OPCODES_IN_RAM
       "Run the hottest 68000 opcode handlers (omega/m68khot.h) and the main loop from SRAM" ON)
if(NOT OMEGA_CPU_SLICE_SLOTS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "OMEGA_CPU_SLICE_SLOTS must be a positive integer")
endif()

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
# HDMI audio (Paula output over HDMI data islands, time-stretched to real
# time). Default ON; with PAL's larger scanout frames the core-1 ring
# shrinks to ~45 KB to make room (see OMEGA_RING_WORDS below).
# OFF: plain DVI output; Paula is still emulated (its interrupts) but not
# mixed.
option(OMEGA_HDMI_AUDIO
       "Send audio over HDMI (data islands in the HSTX blanking; HDMI builds)"
       ON)
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
    src/MemoryBoot.c
    src/Host.c
    src/Planar.c
    src/Presentation.c
)

if(OMEGA_ENABLE_HDMI)
    list(APPEND PLATFORM_SOURCES src/dvi_display.c)
endif()

# The RTG card needs the Fast RAM (its VRAM follows it at 0x400000) and the
# HDMI output (its picture).
set(OMEGA_RTG_ACTIVE OFF)
if(OMEGA_RTG AND OMEGA_FAST_RAM AND OMEGA_ENABLE_HDMI)
    set(OMEGA_RTG_ACTIVE ON)
    list(APPEND PLATFORM_SOURCES src/rtg_display.c omega/Picasso2.c)
elseif(OMEGA_RTG)
    message(STATUS "RTG card left out: it needs OMEGA_FAST_RAM and OMEGA_ENABLE_HDMI")
endif()

if(OMEGA_ENABLE_SDCARD)
    list(APPEND PLATFORM_SOURCES
        src/sd_card.c
        src/sd_diskio.c
        src/ide_sd.c
        src/kicksmash_sd.c
        third_party/fatfs/ff.c
        third_party/fatfs/ffunicode.c
    )
endif()

# Omega emulator core (platform-independent)
set(OMEGA_SOURCES
    omega/Blitter.c
    omega/Chipset.c
    omega/Audio.c
    omega/AudioMix.c
    omega/CIA.c
    omega/CPU.c
    omega/debug.c
    omega/DMA.c
    omega/Floppy.c
    omega/Expansion.c
    omega/Gayle.c
    omega/KickSmash.c
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
set(OMEGA_HDMI_AUDIO_ACTIVE OFF)
if(OMEGA_ENABLE_HDMI AND OMEGA_HDMI_AUDIO)
    set(OMEGA_HDMI_AUDIO_ACTIVE ON)
    target_sources(omega-amiga PRIVATE src/hdmi_island.c src/hdmi_audio.c)
endif()
# Core-1 message ring (32-bit words): the SRAM the frames and the rest leave.
# PAL frames are larger, so PAL with HDMI audio gets 39 KB instead of 61 KB
# (all that fits); HDMI audio also needs 3 KB for Paula's line records
# and their mixer (core 1 code in SRAM).
set(OMEGA_RING_WORDS "" CACHE STRING "Core-1 ring size in words (empty: automatic)")
if(OMEGA_RING_WORDS)
    set(OMEGA_RING_WORDS_VALUE ${OMEGA_RING_WORDS})
elseif(OMEGA_HDMI_AUDIO_ACTIVE AND OMEGA_VIDEO_MODE STREQUAL "PAL")
    set(OMEGA_RING_WORDS_VALUE 10048)
elseif(OMEGA_HDMI_AUDIO_ACTIVE)
    set(OMEGA_RING_WORDS_VALUE 15232)
else()
    set(OMEGA_RING_WORDS_VALUE 15616)
endif()
# Some features need a little of it (SRAM code and data): SD-card builds
# (FatFs, the IDE drive, the KickSmash ROM switcher), the Fast RAM board's
# decoding (Memory.c) and the RTG card (its VRAM writes, its scanout). The
# size is a constant in the ring's index arithmetic, and a multiple of 128
# words keeps that code short (10016 cost 120 bytes).
if(NOT OMEGA_RING_WORDS)
    set(_omega_ring_reserve 0)
    if(OMEGA_ENABLE_SDCARD)
        math(EXPR _omega_ring_reserve "${_omega_ring_reserve} + 32")
    endif()
    if(OMEGA_FAST_RAM)
        math(EXPR _omega_ring_reserve "${_omega_ring_reserve} + 128")
    endif()
    if(OMEGA_RTG_ACTIVE)
        math(EXPR _omega_ring_reserve "${_omega_ring_reserve} + 160")
    endif()
    math(EXPR OMEGA_RING_WORDS_VALUE
         "(${OMEGA_RING_WORDS_VALUE} - ${_omega_ring_reserve}) / 128 * 128")
endif()

# USB host is only built with HDMI (which sets up the clocks for it).
set(OMEGA_USB_HOST_ACTIVE OFF)
if(OMEGA_ENABLE_HDMI AND OMEGA_ENABLE_USB_HOST)
    set(OMEGA_USB_HOST_ACTIVE ON)
    math(EXPR _omega_usb_rem "${OMEGA_SYS_CLK_KHZ} % 48000")
    if(NOT _omega_usb_rem EQUAL 0)
        message(FATAL_ERROR "OMEGA_ENABLE_USB_HOST needs OMEGA_SYS_CLK_KHZ to be a multiple of 48000 (e.g. 336000); it is ${OMEGA_SYS_CLK_KHZ}")
    endif()
endif()

target_compile_definitions(omega-amiga PRIVATE
    PICO_BUILD=1
    PICO_PSRAM_CS_PIN=${PSRAM_CS_PIN}
    PICO_PSRAM_SIZE_BYTES=${PSRAM_SIZE_BYTES}
    PICO_AUTO_DETECT_PSRAM_SIZE=1
    OMEGA_ENABLE_HDMI=$<BOOL:${OMEGA_ENABLE_HDMI}>
    OMEGA_ENABLE_SDCARD=$<BOOL:${OMEGA_ENABLE_SDCARD}>
    BOARD_SD_ROM_PATH="0:/${OMEGA_SD_ROM}"
    OMEGA_ENABLE_FLASH_FLOPPY=$<BOOL:${OMEGA_ENABLE_FLASH_FLOPPY}>
    OMEGA_DF0_INSERT_AT_BOOT=$<BOOL:${OMEGA_DF0_INSERT_AT_BOOT}>
    OMEGA_FAST_RAM=$<BOOL:${OMEGA_FAST_RAM}>
    OMEGA_RTG=$<BOOL:${OMEGA_RTG_ACTIVE}>
    OMEGA_CPU_68020=$<STREQUAL:${OMEGA_CPU},68020>
    OMEGA_VIDEO_STANDARD=${OMEGA_VIDEO_STANDARD_VALUE}
    OMEGA_SYS_CLK_KHZ=${OMEGA_SYS_CLK_KHZ}u
    OMEGA_CPU_CYCLES_PER_SLOT=${OMEGA_CPU_CYCLES_PER_SLOT}
    OMEGA_CPU_SLICE_SLOTS=${OMEGA_CPU_SLICE_SLOTS}
    $<$<NOT:$<BOOL:${OMEGA_SLOT_SKIP}>>:OMEGA_NO_SLOT_SKIP=1>
    OMEGA_ENABLE_USB_HOST=$<BOOL:${OMEGA_USB_HOST_ACTIVE}>
    OMEGA_HDMI_AUDIO=$<BOOL:${OMEGA_HDMI_AUDIO_ACTIVE}>
    OMEGA_AUDIO_OUTPUT=$<BOOL:${OMEGA_HDMI_AUDIO_ACTIVE}>
    OMEGA_REALTIME=$<BOOL:${OMEGA_REALTIME}>
    OMEGA_HOT_OPCODES_IN_RAM=$<BOOL:${OMEGA_HOT_OPCODES_IN_RAM}>
    RING_WORDS=${OMEGA_RING_WORDS_VALUE}u
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

if(OMEGA_USB_HOST_ACTIVE)
    target_sources(omega-amiga PRIVATE src/usb_input.c)
    target_include_directories(omega-amiga PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(omega-amiga tinyusb_host)
endif()

# ── Board-specific stdio ──────────────────────────────────────────────────
pico_enable_stdio_uart(omega-amiga 1)
pico_enable_stdio_usb(omega-amiga 0)
set(OMEGA_STDIO_DESCRIPTION "UART1 GPIO4/5")

# ── Output formats ────────────────────────────────────────────────────────
# Keep the hottest emulator files out of flash .text; the SDK then places
# their code in SRAM (src/ld/default_text_excludes.incl).
if(OMEGA_HOT_CODE_IN_RAM)
    pico_add_linker_script_override_path(omega-amiga
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ld FILES default_text_excludes.incl)
endif()

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
message(STATUS "CPU cycles:  ${OMEGA_CPU_CYCLES_PER_SLOT} per DMA slot (A500: 2), slices of ${OMEGA_CPU_SLICE_SLOTS} slots")
message(STATUS "PSRAM:      ${PSRAM_SIZE_BYTES} bytes, CS GPIO${PSRAM_CS_PIN}")
message(STATUS "HDMI:       ${OMEGA_ENABLE_HDMI}")
message(STATUS "SD card:    ${OMEGA_ENABLE_SDCARD} (Kickstart ${OMEGA_SD_ROM})")
message(STATUS "Flash ADF:  ${OMEGA_ENABLE_FLASH_FLOPPY}")
message(STATUS "Fast RAM:   ${OMEGA_FAST_RAM}")
message(STATUS "RTG card:   ${OMEGA_RTG_ACTIVE}")
message(STATUS "CPU:        ${OMEGA_CPU} (KickSmash banks may override)")
message(STATUS "DF0 at boot:${OMEGA_DF0_INSERT_AT_BOOT}")
message(STATUS "Console:    ${OMEGA_STDIO_DESCRIPTION}")
