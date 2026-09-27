// Omega Amiga emulator – RP2350 bare-metal entry point.
//
// Boot sequence:
//   1. Pico SDK CRT0 runs, initialises clocks / UART (via stdio)
//      and hardware_psram configures QMI PSRAM
//   2. psram_init()     – validate SDK detection and a read/write pattern
//   3. memory_init()    – set up chip/slow RAM in PSRAM, find ROM in flash
//   4. hostInit()       – point framebuffer into PSRAM
//   5. cpu_init()       – reset Musashi 68K, CIA chips
//   6. ChipsetInit()    – reset custom chip register state
//   7. Main loop:       – run DMA + 68K cycles interleaved, call hostDisplay
//                         each PAL frame (19 656 DMA cycles per frame)
//
// ROM placement:
//   Build the final UF2 by combining the firmware UF2 with the Kickstart ROM
//   placed at flash offset 0x200000 (absolute 0x10200000).  See README.md.
//
// Floppy placement:
//   ADF images are MFM-encoded at startup from flash.  Place up to two ADF
//   files at flash offsets defined by ADF0_FLASH_BASE / ADF1_FLASH_BASE.

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/scb.h"

#if OMEGA_ENABLE_HDMI
#include "hardware/psram.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#endif

#include "board_config.h"
#include "psram.h"
#include "Memory.h"
#include "Host.h"
#if OMEGA_ENABLE_SDCARD
#include "sd_card.h"
#endif
#if OMEGA_ENABLE_HDMI
#include "dvi_display.h"
#endif

#include "../omega/CPU.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/DMA.h"
#include "../omega/Floppy.h"
#include "../omega/VideoStandard.h"

#include <string.h>

// ── PAL timing ────────────────────────────────────────────────────────────
// 313 scanlines × 227 DMA cycles/line × 2 (odd+even) ≈ 142 246 cycles/frame
// We batch 200 DMA+CPU pairs per main-loop iteration, matching the original.
#define DMA_CPU_BATCH 200

// ── Flash locations for ADF images (optional) ─────────────────────────────
// Place standard raw ADF images in flash at these absolute addresses. They are
// expanded into each drive's MFM buffer during startup when the feature is on.
// Leave them 0 / unprogrammed to boot without a floppy.
#define ADF0_FLASH_BASE  0x10280000u   // flash offset 0x280000
#define ADF1_FLASH_BASE  0x10480000u   // flash offset 0x480000

void __attribute__((noreturn, used)) hardfault_report(uint32_t *frame) {
    printf("HARDFAULT: pc=%08lx lr=%08lx cfsr=%08lx hfsr=%08lx "
           "mmfar=%08lx bfar=%08lx\n",
           (unsigned long)frame[6], (unsigned long)frame[5],
           (unsigned long)scb_hw->cfsr, (unsigned long)scb_hw->hfsr,
           (unsigned long)scb_hw->mmfar, (unsigned long)scb_hw->bfar);
    for (;;) tight_loop_contents();
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hardfault_report\n");
}

// ── Load floppy images from flash into PSRAM ──────────────────────────────
// The ADF2MFM_from_flash variant reads from XIP flash instead of a file.
static void load_floppy_from_flash(int drive, uint32_t flash_abs_addr) {
    const uint8_t *adf_data = (const uint8_t *)flash_abs_addr;

    // Verify the first sector starts with a valid AmigaDOS block.
    // First two bytes of an OFS ADF should be 0x00 0x00 (T_HEADER).
    // If the flash is blank (0xFF) there is no disk.
    if (adf_data[0] == 0xFF && adf_data[1] == 0xFF) return;

    uint8_t *mfm_buf = floppyInit(drive);
    if (!mfm_buf) return;

    // ADF standard size: 80 cylinders × 2 sides × 11 sectors × 512 B
    const uint32_t adf_size = 80 * 2 * 11 * 512;  // 901 120 bytes
    ADF2MFM_from_mem(adf_data, adf_size, mfm_buf);
    printf("DF%d: loaded from flash 0x%08lx\n", drive, (unsigned long)flash_abs_addr);
}

static void prepare_empty_df0(void) {
    // Kickstart still needs a real, empty DF0 device when no ADF is loaded.
    // Give each track a sync word followed by invalid/zero sector data. This
    // lets trackdisk time out with "no sector header" and display the normal
    // insert-disk screen instead of waiting forever for a sync marker.
    enum { TRACK_BYTES = 12798, CYLINDERS = 80, SIDES = 2 };
    uint8_t *mfm = df[0].mfmData;
    if (!mfm) return;

    memset(mfm, 0, PSRAM_FLOPPY_SIZE);
    for (int cylinder = 0; cylinder < CYLINDERS; ++cylinder) {
        for (int side = 0; side < SIDES; ++side) {
            uint32_t offset = (uint32_t)(cylinder * SIDES + side) * TRACK_BYTES;
            mfm[offset] = 0x44;
            mfm[offset + 1] = 0x89;
        }
    }
}

static bool load_kickstart_from_sd(void) {
#if OMEGA_ENABLE_SDCARD
    size_t rom_size = 0;
    uint8_t *rom_cache = psram_ptr(PSRAM_SD_ROM_OFFSET);
    if (!sd_card_mount() ||
        !sd_card_load_file(BOARD_SD_ROM_PATH, rom_cache, PSRAM_SD_ROM_SIZE,
                           &rom_size)) {
        printf("ROM: using flash fallback\n");
        return false;
    }
    if (!memory_set_rom(rom_cache, (uint32_t)rom_size)) {
        printf("ROM: %s must be a valid 256 KB or 512 KB Kickstart\n",
               BOARD_SD_ROM_PATH);
        printf("ROM: using flash fallback\n");
        return false;
    }
    printf("ROM: loaded %s from SD (%lu KB)\n", BOARD_SD_ROM_PATH,
           (unsigned long)(rom_size >> 10));
    return true;
#else
    return false;
#endif
}

// ── Second core: runs the 68K CPU (future use) ────────────────────────────
// Phase 1: everything runs on core 0.  Un-comment core1_entry to enable
// dual-core operation once the shared-state locking is implemented.
//
// static void core1_entry(void) {
//     for (;;) cpu_execute();
// }

#if OMEGA_ENABLE_HDMI
static void prepare_hdmi_clock(void) {
#if OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL
    const uint32_t dvi_clock_khz = 270000u;
#else
    const uint32_t dvi_clock_khz = 252000u;
#endif

    // PicoDVI serialises one TMDS bit per system-clock cycle. Raise the core
    // voltage before overclocking, then recalculate and apply the SDK PSRAM
    // timing for the faster QMI clock before touching external memory.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    if (!set_sys_clock_khz(dvi_clock_khz, true))
        panic("Unable to select the PicoDVI system clock");
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != PICO_OK ||
        psram_reinitialize() != PICO_OK)
        panic("Unable to retime PSRAM for PicoDVI");
}
#endif

int main(void) {
    // 1. Clock and stdio. Normal builds retain the SDK's 150 MHz startup
    // clock; PicoDVI builds select their video bit clock and retime PSRAM.
#if OMEGA_ENABLE_HDMI
    prepare_hdmi_clock();
#endif
#if OMEGA_STDIO_USB
    stdio_init_all();
    // Let the host enumerate USB CDC before printing the bring-up log.
    sleep_ms(1500);
#else
    stdio_uart_init_full(BOARD_UART_ID, BOARD_UART_BAUD,
                         BOARD_UART_TX_PIN, BOARD_UART_RX_PIN);
#endif
    printf("\n\nOmega/RP2350 – Amiga emulator\n");
    printf("Sys clock: %lu kHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000));
    printf("Video: %s (%d lines, %.3f Hz)\n",
           OMEGA_VIDEO_NAME, OMEGA_VIDEO_FRAME_LINES,
           (double)OMEGA_VIDEO_RATE_NUMERATOR /
           OMEGA_VIDEO_RATE_DENOMINATOR);

#if OMEGA_ENABLE_HDMI
    // Launch core 1 first, but hold it before scanout until PSRAM is ready.
    dvi_display_init();
    multicore_fifo_pop_blocking();
#endif

    // 2. PSRAM was configured by hardware_psram before main(). Validate it.
    if (!psram_init()) {
        printf("PSRAM: unsupported or not responding; halted\n");
        for (;;) tight_loop_contents();
    }
    printf("PSRAM initialised at 0x%08x (%u MB)\n", PSRAM_BASE, PSRAM_SIZE >> 20);

    // 3. Memory (chip RAM, slow RAM, flash ROM fallback, optional SD override)
    memory_init();
    bool sd_rom_active = load_kickstart_from_sd();

    // 4. Host (framebuffer, display hardware)
    hostInit();
#if OMEGA_ENABLE_HDMI
    dvi_display_start();
#endif

    // 5. Emulator core
    cpu_init();
    ChipsetInit();

    // 6. Floppy images (optional). Keep this disabled while isolating HDMI;
    // Kickstart boots without a disk and no MFM conversion runs at startup.
    for (int drive = 0; drive < 4; ++drive)
        floppyInit(drive);
#if OMEGA_ENABLE_FLASH_FLOPPY
    load_floppy_from_flash(0, ADF0_FLASH_BASE);
#if OMEGA_ENABLE_HDMI
    (void)sd_rom_active;
    printf("DF1: disabled (its PSRAM region holds HDMI scanout buffers)\n");
#else
    if (!sd_rom_active) load_floppy_from_flash(1, ADF1_FLASH_BASE);
    else printf("DF1: disabled (its PSRAM region holds the SD ROM cache)\n");
#endif
#else
    (void)sd_rom_active;
    prepare_empty_df0();
    printf("DF0/DF1: flash ADF loading disabled\n");
#endif

    printf("Entering emulation loop\n");

    // ── Main loop ─────────────────────────────────────────────────────────
    for (;;) {
        for (int i = 0; i < DMA_CPU_BATCH; ++i) {
            dma_execute();
            cpu_execute();
        }
        // hostDisplay is called exactly once per VBL by the DMA engine.  It
        // converts the intermediate beam raster into the 640x400 output.
    }

    // Unreachable
    return 0;
}
