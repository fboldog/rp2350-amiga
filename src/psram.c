// RP2350 PSRAM setup using the Pico SDK hardware_psram driver.
//
// Boot sequence (SDK runtime_init_setup_psram):
//   1. Detects APS6404L on QMI CS1/GPIO47, stores 8 MB in flash_devinfo.
//   2. Calls psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ, ...) using
//      the boot clock (~150 MHz) → divisor and rxdelay tuned for that clock.
//   3. Calls psram_reinitialize() → QPI mode entered, QMI CS1 registers set,
//      XIP_CTRL_WRITABLE_M1 enabled, RP2350-E14 pad-isolation fix applied.
//
// main() then calls set_board_sys_clock() to raise clk_sys to 270 MHz.
// That leaves QMI CS1 timing stale: old divisor gives a higher PSRAM SCLK
// than the target, and rxdelay (in half-system-clock units) is too short.
//
// psram_init() fixes timing by rewriting QMI M1 registers directly after
// the clock change.  It does NOT call psram_reinitialize / flash_start_xip,
// which would flush and re-initialise flash XIP; at 270 MHz the W25Q128
// (max 104 MHz) would fail cold-cache fetches.
//
// ATRANS: ATRANS[0..3] always map to CS0 (flash), ATRANS[4..7] always map to
// CS1 (PSRAM).  The BASE field is the SPI offset within each CS's address
// space (16 MB wrap).  Reset values are the identity mapping and are already
// correct for an 8 MB PSRAM at virtual 0x11000000:
//   ATRANS[4] = 0x04000000  (SIZE=1024×4KB=4MB, BASE=0)    → PSRAM 0-4MB
//   ATRANS[5] = 0x04000400  (SIZE=1024, BASE=1024=4MB)      → PSRAM 4-8MB
// No ATRANS changes are needed.
//
// WHY NOT psram_send_quad_enable() (direct SPI mode):
//   QMI direct mode deasserts CS0, which exits the W25Q128 from its
//   continuous XIP read state.  Later flash reads stall the QMI
//   indefinitely because the flash no longer responds to the XIP command.
//   runtime_init_setup_psram already put PSRAM in QPI mode via its
//   psram_initialize_internal callback; we must not re-enter direct mode.
//
// MULTICORE QMI RESET HAZARD:
//   When core 1 is launched via multicore_launch_core1(), the SDK's
//   core1_wrapper calls runtime_run_per_core_initializers() which includes
//   runtime_init_per_core_bootrom_reset().  That function calls the ROM's
//   bootrom_state_reset(BOOTROM_STATE_RESET_CURRENT_CORE), which resets QMI
//   M1 registers to SPI-single defaults (M1_RFMT → 0x00001000 = single-wire
//   03h mode).  The SDK's flash_set_qmi_cs1_setup_function callback is NOT
//   called by the ROM; M1 stays in SPI mode.
//
//   Fix (main.c): after dvi_display_init(), core 0 waits for core 1's
//   per-core init to finish (via FIFO), then calls psram_reinstate_m1() to
//   restore QUAD mode before memory_init() starts any PSRAM writes.

#include "psram.h"
#include "board_config.h"

#include "hardware/psram.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include <stdio.h>

// APS6404L SPI command codes
#define PSRAM_QUAD_READ_CMD    0xEBu
#define PSRAM_QUAD_WRITE_CMD   0x38u

// Stored QMI M1 register values, populated by psram_init().
// psram_reinstate_m1() re-applies them after any QMI reset.
static uint32_t s_m1_timing;
static uint32_t s_m1_rfmt;
static uint32_t s_m1_rcmd;
static uint32_t s_m1_wfmt;
static uint32_t s_m1_wcmd;

// Re-apply QMI M1 QUAD-mode registers without entering direct mode.
// Must be in RAM: called from flash_rp2350_restore_qmi_cs1 (the registered
// CS1 setup callback) when flash XIP is briefly suspended, and from main.c
// after core 1's per-core init resets QMI M1 to SPI defaults.
void __not_in_flash_func(psram_reinstate_m1)(void) {
    qmi_hw->m[1].timing = s_m1_timing;
    qmi_hw->m[1].rfmt   = s_m1_rfmt;
    qmi_hw->m[1].rcmd   = s_m1_rcmd;
    qmi_hw->m[1].wfmt   = s_m1_wfmt;
    qmi_hw->m[1].wcmd   = s_m1_wcmd;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

bool psram_init(void) {
    if (!psram_is_available()) {
        printf("PSRAM: SDK initialization did not detect a device\n");
        return false;
    }

    size_t detected_size = psram_get_size();
    printf("PSRAM: SDK detected %lu MB\n",
           (unsigned long)(detected_size >> 20));
    if (detected_size < PSRAM_SIZE) {
        printf("PSRAM: insufficient – need %lu MB\n",
               (unsigned long)(PSRAM_SIZE >> 20));
        return false;
    }

    // Sync SDK internal state for any future flash_start_xip (e.g. flash erase).
    int rc = psram_configure_params(BOARD_PSRAM_MAX_SCK_HZ,
                                    PICO_DEFAULT_PSRAM_MAX_SELECT,
                                    PICO_DEFAULT_PSRAM_MIN_DESELECT);
    if (rc != PICO_OK) {
        printf("PSRAM: psram_configure_params failed (%d)\n", rc);
        return false;
    }

    // Compute timing values for the post-overclock system clock.
    uint32_t sys_hz    = clock_get_hz(clk_sys);
    uint32_t divisor   = (sys_hz + BOARD_PSRAM_MAX_SCK_HZ - 1u) / BOARD_PSRAM_MAX_SCK_HZ;
    if (divisor == 1u && sys_hz > 100000000u) divisor = 2u;
    uint32_t rxdelay   = divisor + ((sys_hz / divisor > 100000000u) ? 1u : 0u);
    uint32_t period_fs = (uint32_t)(1000000000000000ull / sys_hz);
    uint32_t max_sel   = (uint32_t)((8000ull * 1000000ull) / (64ull * period_fs));
    uint32_t min_desel = (PICO_DEFAULT_PSRAM_MIN_DESELECT * 1000000u + period_fs - 1u) / period_fs;
    if (min_desel > (divisor + 1u) / 2u) min_desel -= (divisor + 1u) / 2u;
    else min_desel = 0u;

    // ── RP2350-E14 workaround ────────────────────────────────────────────────
    hw_clear_bits(&pads_bank0_hw->io[PICO_PSRAM_CS_PIN], PADS_BANK0_GPIO0_ISO_BITS);
    gpio_set_function(PICO_PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);

    // ── QMI M1 registers for QPI PSRAM ──────────────────────────────────────
    // PSRAM is already in QPI mode from runtime_init; do NOT re-enter direct
    // mode or re-send 0x35 (that would break flash XIP on CS0).
    qmi_hw->m[1].timing =
        (1u                                 << QMI_M1_TIMING_COOLDOWN_LSB)   |
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB)  |
        (max_sel   << QMI_M1_TIMING_MAX_SELECT_LSB)   |
        (min_desel << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (rxdelay   << QMI_M1_TIMING_RXDELAY_LSB)      |
        (divisor   << QMI_M1_TIMING_CLKDIV_LSB);

    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q   << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q     << QMI_M1_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q   << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q    << QMI_M1_RFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q     << QMI_M1_RFMT_DATA_WIDTH_LSB)   |
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8     << QMI_M1_RFMT_PREFIX_LEN_LSB)   |
        (QMI_M1_RFMT_DUMMY_LEN_VALUE_24     << QMI_M1_RFMT_DUMMY_LEN_LSB)    |
        (QMI_M1_RFMT_SUFFIX_LEN_VALUE_NONE  << QMI_M1_RFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].rcmd = PSRAM_QUAD_READ_CMD << QMI_M1_RCMD_PREFIX_LSB;

    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q   << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q     << QMI_M1_WFMT_ADDR_WIDTH_LSB)   |
        (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q   << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q    << QMI_M1_WFMT_DUMMY_WIDTH_LSB)  |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q     << QMI_M1_WFMT_DATA_WIDTH_LSB)   |
        (QMI_M1_WFMT_PREFIX_LEN_VALUE_8     << QMI_M1_WFMT_PREFIX_LEN_LSB)   |
        (QMI_M1_WFMT_DUMMY_LEN_VALUE_NONE   << QMI_M1_WFMT_DUMMY_LEN_LSB)    |
        (QMI_M1_WFMT_SUFFIX_LEN_VALUE_NONE  << QMI_M1_WFMT_SUFFIX_LEN_LSB);
    qmi_hw->m[1].wcmd = PSRAM_QUAD_WRITE_CMD << QMI_M1_WCMD_PREFIX_LSB;

    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    // Save M1 register values so psram_reinstate_m1() can restore them.
    // Also register it as the CS1 callback so flash_rp2350_restore_qmi_cs1
    // calls it after any flash_exit_xip (which resets M1 to SPI defaults).
    s_m1_timing = qmi_hw->m[1].timing;
    s_m1_rfmt   = qmi_hw->m[1].rfmt;
    s_m1_rcmd   = qmi_hw->m[1].rcmd;
    s_m1_wfmt   = qmi_hw->m[1].wfmt;
    s_m1_wcmd   = qmi_hw->m[1].wcmd;
    flash_set_qmi_cs1_setup_function(psram_reinstate_m1);

    // Read back M1 timing to verify writes were accepted.
    uint32_t got_timing = qmi_hw->m[1].timing;
    uint32_t got_clkdiv   = (got_timing >> QMI_M1_TIMING_CLKDIV_LSB)   & 0x3fu;
    uint32_t got_rxdelay  = (got_timing >> QMI_M1_TIMING_RXDELAY_LSB)  & 0x7u;
    printf("PSRAM: M1 timing: CLKDIV=%lu RXDELAY=%lu (wanted %lu/%lu), "
           "%lu MHz sys → SCLK=%lu MHz\n",
           (unsigned long)got_clkdiv, (unsigned long)got_rxdelay,
           (unsigned long)divisor,    (unsigned long)rxdelay,
           (unsigned long)(sys_hz / 1000000u),
           (unsigned long)(sys_hz / (got_clkdiv ? got_clkdiv : 1u) / 1000000u));

    // ── Probe via the no-cache XIP alias ────────────────────────────────────
    // Read from the no-cache/no-alloc alias (0x15000000 = PSRAM_BASE+0x4000000)
    // to bypass the XIP cache and exercise the actual QMI PSRAM path.
    // Write via the same alias for symmetry (write-through for the cache too).
    //
    // To avoid a NOCP-during-printf triggered by the probe value, we compare
    // inline and print only a pass/fail string.
    volatile uint32_t *probe =
        (volatile uint32_t *)(PSRAM_BASE + 0x04000000u);

    // Validate the no-cache path works at all by reading from flash first.
    volatile uint32_t *nc_flash = (volatile uint32_t *)0x14000000u;
    uint32_t flash_word = nc_flash[0];
    printf("PSRAM: nc-flash[0]=0x%08lx (should match 0x10000000)\n",
           (unsigned long)flash_word);

    probe[0] = 0x51A7C000u;
    probe[1] = 0xDEADBEEFu;
    __dsb();

    uint32_t val0 = probe[0];
    uint32_t val1 = probe[1];

    bool ok = (val0 == 0x51A7C000u) && (val1 == 0xDEADBEEFu);
    if (ok) {
        printf("PSRAM: probe passed\n");
    } else {
        // Print pass/fail separately from values to isolate any NOCP trigger.
        printf("PSRAM: probe FAILED\n");
        printf("PSRAM: val0=%c val1=%c\n",
               val0 == 0x51A7C000u ? 'Y' : 'N',
               val1 == 0xDEADBEEFu ? 'Y' : 'N');
    }

    return ok;
}
