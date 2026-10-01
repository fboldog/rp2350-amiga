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
//   Only DF0 is used. Raw ADF images live back to back in flash from
//   ADF0_FLASH_BASE (up to 15 on 16 MB); each boot mounts the next one.
//   Only the active track is encoded, into a PSRAM track buffer.

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/qmi.h"

#if OMEGA_ENABLE_HDMI
#include "hardware/pll.h"
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
#include "../omega/m68k.h"
#include "../omega/Chipset.h"
#include "../omega/CIA.h"
#include "../omega/DMA.h"
#include "../omega/Floppy.h"
#include "../omega/VideoStandard.h"

#include <string.h>

// ── PAL timing ────────────────────────────────────────────────────────────
// 313 scanlines × 227 DMA cycles/line × 2 (odd+even) ≈ 142 246 cycles/frame
// The 68000 runs in slices of OMEGA_CPU_SLICE_SLOTS DMA slots
// (-DOMEGA_CPU_SLICE_SLOTS=<n>): longer slices cost fewer Musashi entries per
// instruction, shorter ones let the CPU see the beam move in finer steps.
// The CPU:DMA cycle ratio is the same either way. The main loop handles
// about 200 slots per iteration (whole slices) between input polls.
#ifndef OMEGA_CPU_SLICE_SLOTS
#define OMEGA_CPU_SLICE_SLOTS 8
#endif
#define DMA_CPU_BATCH \
    (((200 + OMEGA_CPU_SLICE_SLOTS - 1) / OMEGA_CPU_SLICE_SLOTS) * \
     OMEGA_CPU_SLICE_SLOTS)
// 68000 cycles per DMA slot (colour clock). A real A500 runs 2 (7.09 MHz
// CPU, 3.55 MHz colour clock); Omega used 16. Set with
// -DOMEGA_CPU_CYCLES_PER_SLOT=<n> (CMakeLists.txt); the default here covers
// builds outside CMake.
#ifndef OMEGA_CPU_CYCLES_PER_SLOT
#define OMEGA_CPU_CYCLES_PER_SLOT 2
#endif

// ── Flash location of the DF0 ADF images (optional) ──────────────────────
// Standard raw ADF images are placed back to back from this absolute address
// (tools/combine_uf2.py --adf a.adf --adf b.adf ...). The first slot whose
// first word is erased flash ends the list; leave slot 0 unprogrammed to
// boot without a floppy.
#define ADF0_FLASH_BASE  0x10280000u   // flash offset 0x280000
#define ADF_SLOTS ((PICO_FLASH_SIZE_BYTES - (ADF0_FLASH_BASE - XIP_BASE)) / \
                   FLOPPY_ADF_SIZE)  // 15 with 16 MB of flash
// Disk rotation: every boot, reset included, mounts the next slot. The
// position is kept in PSRAM, which stays powered through a RUN-pin reset
// (the POWMAN scratch registers do not survive one) and loses its contents
// on a power cycle, so power-on starts at the first disk. It is accessed
// uncached, so nothing is left in the XIP cache at reset, and guarded by a
// magic and a complement against random power-on contents.
#define ADF_ROTATION_MAGIC 0xadf0b007u

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

// ── Mount a DF0 ADF image from flash ─────────────────────────────────────
static const uint8_t *adf_slot(unsigned slot) {
    return (const uint8_t *)(ADF0_FLASH_BASE + slot * FLOPPY_ADF_SIZE);
}

static unsigned count_adf_slots(void) {
    unsigned count = 0;
    // Blank flash (0xFFFFFFFF) means no image has been programmed there.
    while (count < ADF_SLOTS &&
           *(const uint32_t *)adf_slot(count) != 0xffffffffu)
        ++count;
    return count;
}

// Returns the slot for this boot and advances the rotation.
static unsigned next_adf_slot(unsigned count) {
    volatile uint32_t *state = (volatile uint32_t *)(
        PSRAM_BASE - XIP_BASE + XIP_NOCACHE_NOALLOC_BASE +
        BOARD_MAP_BOOT_STATE_OFFSET);
    unsigned slot = 0;
    if (state[0] == ADF_ROTATION_MAGIC && state[1] == ~state[2])
        slot = state[1] % count;
    const uint32_t next = (slot + 1u) % count;
    state[1] = next;
    state[2] = ~next;
    state[0] = ADF_ROTATION_MAGIC;
    return slot;
}

static bool load_df0_from_flash(void) {
    const unsigned count = count_adf_slots();
    if (count == 0)
        return false;
    const unsigned slot = next_adf_slot(count);
    const uint8_t *adf_data = adf_slot(slot);
    if (!floppyMountADF(0, adf_data, FLOPPY_ADF_SIZE))
        return false;
    printf("DF0: flash ADF %u of %u (0x%08lx) mounted with a %u-byte PSRAM "
           "track buffer\n", slot + 1u, count, (unsigned long)adf_data,
           FLOPPY_MFM_TRACK_SIZE);
    return true;
}

#if OMEGA_ENABLE_FLASH_FLOPPY
static bool df0_image_ready;
static bool disk_button_raw;
static bool disk_button_stable;
static bool disk_insert_pending;
static uint32_t disk_button_change_us;

static bool disk_button_pressed(void) {
    bool level = gpio_get(BOARD_USER_BUTTON_PIN);
#if BOARD_USER_BUTTON_ACTIVE_LOW
    return !level;
#else
    return level;
#endif
}

static void disk_button_init(void) {
    gpio_init(BOARD_USER_BUTTON_PIN);
    gpio_set_dir(BOARD_USER_BUTTON_PIN, GPIO_IN);
#if BOARD_USER_BUTTON_ACTIVE_LOW
    gpio_pull_up(BOARD_USER_BUTTON_PIN);
#else
    gpio_pull_down(BOARD_USER_BUTTON_PIN);
#endif
    disk_button_raw = disk_button_pressed();
    disk_button_stable = disk_button_raw;
    disk_button_change_us = time_us_32();
    printf("DF0: KEY GPIO%u inserts/ejects the mounted disk\n",
           BOARD_USER_BUTTON_PIN);
}

static void disk_button_poll(void) {
    bool pressed = disk_button_pressed();
    uint32_t now = time_us_32();
    if (pressed != disk_button_raw) {
        disk_button_raw = pressed;
        disk_button_change_us = now;
    }
    if (pressed != disk_button_stable &&
        (uint32_t)(now - disk_button_change_us) >= 30000u) {
        disk_button_stable = pressed;
        if (pressed && df0_image_ready) {
            if (df[0].hasDisk) {
                floppyInsert(0);
            } else {
                disk_insert_pending = true;
                printf("DF0: insertion requested\n");
            }
        }
    }

    // Kickstart ignores disk changes until the 32-pulse drive-ID sequence is
    // complete. Retain an early button press and apply it as soon as valid.
    if (disk_insert_pending && !df[0].hasDisk && df[0].idMode == 0) {
        floppyInsert(0);
        if (df[0].hasDisk)
            disk_insert_pending = false;
    }
}
#endif

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
    // HSTX gets its clock from PLL_USB, retuned to the TMDS bit rate, so
    // clk_sys can be chosen for the emulator (OMEGA_SYS_CLK_KHZ) and PSRAM
    // runs close to its 133 MHz limit. PLL_USB no longer provides 48 MHz:
    // clk_peri moves to PLL_SYS and the unused USB and ADC clocks stop.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    if (!set_sys_clock_khz(OMEGA_SYS_CLK_KHZ, true))
        panic("Unable to select the system clock");
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    // clk_peri (UART, SPI) stays at or below 150 MHz; its divider is 1..3.
    const uint32_t peri_div = (sys_hz + 150000000u - 1u) / 150000000u;
    if (!clock_configure(clk_peri, 0,
                         CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                         sys_hz, sys_hz / peri_div))
        panic("Unable to move clk_peri to PLL_SYS");
    clock_stop(clk_usb);
    clock_stop(clk_adc);
    uint vco_hz, post_div1, post_div2;
    if (!check_sys_clock_hz(dvi_display_bit_clock_khz() * 1000u,
                            &vco_hz, &post_div1, &post_div2))
        panic("No PLL_USB setting for the HDMI bit rate");
    pll_init(pll_usb, 1, vco_hz, post_div1, post_div2);
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                               PICO_DEFAULT_PSRAM_MAX_SELECT,
                               PICO_DEFAULT_PSRAM_MIN_DESELECT) != PICO_OK ||
        psram_reinitialize() != PICO_OK)
        panic("Unable to retime PSRAM for the system clock");
}
#endif

int main(void) {
    // 1. Clock and stdio. Normal builds retain the SDK's 150 MHz startup
    // clock; HDMI builds select OMEGA_SYS_CLK_KHZ, drive HSTX from PLL_USB
    // and retime PSRAM.
#if OMEGA_ENABLE_HDMI
    prepare_hdmi_clock();
#endif
    stdio_uart_init_full(BOARD_UART_ID, BOARD_UART_BAUD,
                         BOARD_UART_TX_PIN, BOARD_UART_RX_PIN);
    printf("\n\nOmega/RP2350 – Amiga emulator\n");
    printf("Sys clock: %lu kHz\n", (unsigned long)(clock_get_hz(clk_sys) / 1000));
#if OMEGA_ENABLE_HDMI
    printf("PSRAM clock: %lu kHz (QMI divisor %lu)\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000 /
                           ((qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >>
                            QMI_M1_TIMING_CLKDIV_LSB)),
           (unsigned long)((qmi_hw->m[1].timing & QMI_M1_TIMING_CLKDIV_BITS) >>
                           QMI_M1_TIMING_CLKDIV_LSB));
#endif
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
    load_kickstart_from_sd();

    // 4. Host (framebuffer, display hardware)
    hostInit();
#if OMEGA_ENABLE_HDMI
    dvi_display_start();
#endif

    // 5. Emulator core
    cpu_init();
    ChipsetInit();

    // 6. Floppy. Only DF0 takes disks; DF1-DF3 are initialised as
    // unconnected drives so Kickstart's drive-ID probe skips them.
    for (int drive = 0; drive < 4; ++drive)
        floppyInit(drive);
#if OMEGA_ENABLE_FLASH_FLOPPY
    bool df0_loaded = load_df0_from_flash();
    df0_image_ready = df0_loaded;
    disk_button_init();
#if OMEGA_DF0_INSERT_AT_BOOT
    if (df0_loaded) {
        df[0].hasDisk = 1;
        df[0].pra &= 0xFB;
        printf("DF0: disk inserted at boot; KEY ejects/reinserts it\n");
    }
#else
    if (df0_loaded)
        printf("DF0: no disk inserted; press KEY after Kickstart starts\n");
#endif
#else
    prepare_empty_df0();
    printf("DF0: flash ADF loading disabled\n");
#endif

    printf("Entering emulation loop\n");

    // ── Main loop ─────────────────────────────────────────────────────────
    for (;;) {
        // Run the 68000 in slices of several DMA slots: entering Musashi for
        // 16 cycles every slot spent more time in call overhead than in the
        // 1-3 instructions it ran. The CPU:DMA cycle ratio is unchanged; the
        // CPU just observes chipset state at slice granularity.
        // Musashi finishes whole instructions, so it can overrun the budget;
        // the overrun is carried into the next slice to keep the ratio exact.
        static int cpu_cycle_balance;
        for (int i = 0; i < DMA_CPU_BATCH; i += OMEGA_CPU_SLICE_SLOTS) {
            dma_run(OMEGA_CPU_SLICE_SLOTS);
            cpu_cycle_balance +=
                OMEGA_CPU_CYCLES_PER_SLOT * OMEGA_CPU_SLICE_SLOTS;
            if (cpu_cycle_balance > 0)
                cpu_cycle_balance -= m68k_execute(cpu_cycle_balance);
        }
#if OMEGA_ENABLE_FLASH_FLOPPY
        disk_button_poll();
#endif
        // hostDisplay is called exactly once per VBL by the DMA engine.  It
        // converts the intermediate beam raster into the 640x400 output.
    }

    // Unreachable
    return 0;
}
