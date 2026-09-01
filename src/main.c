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

#include "psram.h"
#include "Memory.h"
#include "Host.h"

#include "../omega/CPU.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/DMA.h"
#include "../omega/Floppy.h"

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
// RP2350 default: 150 MHz.  Push to 250 MHz for headroom.
// PSRAM timing in psram.c is set for ≤ 133 MHz QSPI (clkdiv=2 → 125 MHz).
// Bump clkdiv to 2 gives ~125 MHz PSRAM which is within spec.
#define OVERCLOCK_KHZ  250000u

static void set_sys_clock_250mhz(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_15);
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
    stdio_uart_init_full(uart0, 115200, 0 /*TX GPIO*/, 1 /*RX GPIO*/);
    printf("\n\nOmega/RP2350 – Amiga emulator\n");
    printf("Sys clock: %lu kHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000));

    // 2. PSRAM
    psram_init();
    printf("PSRAM initialised at 0x%08x (%u MB)\n", PSRAM_BASE, PSRAM_SIZE >> 20);

    // 3. Memory (chip RAM, slow RAM, ROM from flash)
    memory_init();

    // 4. Host (framebuffer, display hardware)
    hostInit();

    // 5. Emulator core
    cpu_init();
    ChipsetInit();

    // 6. Floppy images (optional)
    load_floppy_from_flash(0, ADF0_FLASH_BASE);
    load_floppy_from_flash(1, ADF1_FLASH_BASE);

    printf("Entering emulation loop\n");

    // ── Main loop ─────────────────────────────────────────────────────────
    for (;;) {
        for (int i = 0; i < DMA_CPU_BATCH; ++i) {
            dma_execute();
            cpu_execute();
        }
        // hostDisplay is called by the DMA engine each VBL via the
        // Chipset/DMA layer (see DMA.c evenCycle / VBL handler).
        // Call it here as a fallback so the framebuffer is periodically
        // pushed to the display even if VBL signalling is not yet wired.
        hostDisplay();
    }

    // Unreachable
    return 0;
}
