// Omega Amiga emulator – RP2350 bare-metal entry point.
//
// Boot sequence:
//   1. Pico SDK CRT0 runs, initialises clocks / UART (via stdio)
//   2. psram_init()     – configure QMI CS1 for 8 MB APS6404L PSRAM
//   3. memory_init()    – set up chip/slow RAM in PSRAM, find ROM in flash
//   4. hostInit()       – point framebuffer into PSRAM, init display HW
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
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/scb.h"

#include "board_config.h"
#include "psram.h"
#include "Memory.h"
#include "Host.h"
#include "sd_card.h"

#include "../omega/CPU.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/DMA.h"
#include "../omega/Floppy.h"
#include "../omega/VideoStandard.h"

// ── PAL timing ────────────────────────────────────────────────────────────
// 313 scanlines × 227 DMA cycles/line × 2 (odd+even) ≈ 142 246 cycles/frame
// We batch 200 DMA+CPU pairs per main-loop iteration, matching the original.
#define DMA_CPU_BATCH 200

// ── Flash locations for ADF images (optional) ─────────────────────────────
// Place MFM-pre-encoded ADF binaries in flash at these absolute addresses.
// Leave them 0 / unprogrammed to boot without a floppy.
#define ADF0_FLASH_BASE  0x10280000u   // flash offset 0x280000
#define ADF1_FLASH_BASE  0x10480000u   // flash offset 0x480000
#define ADF_FLASH_SIZE   0x200000u     // 2 MB per drive (MFM encoded)

// ── Overclock ─────────────────────────────────────────────────────────────
// RP2350 default: 150 MHz. OMEGA_BOARD_SYS_CLK_KHZ is the working
// point (250 MHz).  PSRAM QSPI = sys / BOARD_PSRAM_CLKDIV, kept ≤ 133 MHz.
#define OVERCLOCK_KHZ  OMEGA_BOARD_SYS_CLK_KHZ

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

static void set_sys_clock_250mhz(void) {
    vreg_set_voltage(BOARD_VREG_VOLTAGE);
    sleep_ms(10);
    set_sys_clock_khz(OVERCLOCK_KHZ, true);
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

static bool load_kickstart_from_sd(void) {
#if BOARD_HAS_SDCARD
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

int main(void) {
    // 1. Clock and UART
    set_sys_clock_250mhz();
    stdio_uart_init_full(BOARD_UART_ID, BOARD_UART_BAUD,
                         BOARD_UART_TX_PIN, BOARD_UART_RX_PIN);
    printf("\n\nOmega/RP2350 – Amiga emulator\n");
    printf("Sys clock: %lu kHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000));
    printf("Video: %s (%d lines, %.3f Hz)\n",
           OMEGA_VIDEO_NAME, OMEGA_VIDEO_FRAME_LINES,
           (double)OMEGA_VIDEO_RATE_NUMERATOR /
           OMEGA_VIDEO_RATE_DENOMINATOR);

    // 2. PSRAM
    if (!psram_init()) {
        printf("PSRAM: unsupported or not responding; halted\n");
        for (;;) tight_loop_contents();
    }
    printf("PSRAM initialised at 0x%08x (%u MB)\n", PSRAM_BASE, PSRAM_SIZE >> 20);

    // 3. Memory (chip RAM, slow RAM, flash ROM fallback, then SD override)
    memory_init();
    bool sd_rom_active = load_kickstart_from_sd();

    // 4. Host (framebuffer, display hardware)
    hostInit();

    // 5. Emulator core
    cpu_init();
    ChipsetInit();

    // 6. Floppy images (optional)
    load_floppy_from_flash(0, ADF0_FLASH_BASE);
    if (!sd_rom_active) load_floppy_from_flash(1, ADF1_FLASH_BASE);
    else printf("DF1: disabled (its PSRAM region holds the SD ROM cache)\n");

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
