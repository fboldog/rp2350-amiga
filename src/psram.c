// PSRAM initialisation for RP2350 – APS6404L on QMI CS1
// Based on Raspberry Pi / Pimoroni RP2350 examples (SDK 2.x)
//
// After this call, 8MB of PSRAM is accessible at PSRAM_BASE (0x11000000).

#include "psram.h"
#include "hardware/structs/qmi.h"
#include "hardware/xip_cache.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"

// QMI register field helpers (SDK 2.x symbols)
#ifndef QMI_M1_TIMING_CLKDIV_LSB
#define QMI_M1_TIMING_CLKDIV_LSB    0
#endif

static void _psram_set_qspi_timing(void) {
    // System clock (typically 150 MHz on Pico Plus 2 after PLL).
    // APS6404L max read clock: 133 MHz in Quad mode → clkdiv ≥ 2 at 150 MHz.
    const uint32_t clkdiv = 2;

    // Timing register: clkdiv, min-deselect, max-select, rxdelay
    qmi_hw->m[1].timing =
        (clkdiv    << QMI_M1_TIMING_CLKDIV_LSB)        |
        (2u        << QMI_M1_TIMING_MIN_DESELECT_LSB)   |  // ≥12 ns
        (0x3fu     << QMI_M1_TIMING_MAX_SELECT_LSB)     |  // max CS active
        (2u        << QMI_M1_TIMING_SELECT_HOLD_LSB)    |
        (1u        << QMI_M1_TIMING_RXDELAY_LSB);

    // Read format: Quad I/O, EBh command, 6 dummy cycles
    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_RFMT_ADDR_WIDTH_LSB)   |
        (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M1_RFMT_DUMMY_WIDTH_LSB)  |
        (6u << QMI_M1_RFMT_DUMMY_LEN_LSB)                                   |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_RFMT_DATA_WIDTH_LSB)   |
        (1u << QMI_M1_RFMT_PREFIX_LEN_LSB);   // 8-bit prefix

    qmi_hw->m[1].rcmd = 0xEB;   // Quad Fast Read command

    // Write format: Quad, 38h command, no dummy
    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M1_WFMT_ADDR_WIDTH_LSB)   |
        (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M1_WFMT_DATA_WIDTH_LSB)   |
        (1u << QMI_M1_WFMT_PREFIX_LEN_LSB);

    qmi_hw->m[1].wcmd = 0x38;   // Quad Write command
}

static void _psram_send_direct_byte(uint8_t byte) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_TXFULL_BITS) tight_loop_contents();
    qmi_hw->direct_tx = byte;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)  tight_loop_contents();
}

static void _psram_reset(void) {
    // Assert CS1, send Reset Enable (66h) then Reset (99h)
    hw_set_bits(&qmi_hw->direct_csr,
                QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    _psram_send_direct_byte(0x66);  // Reset Enable
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    hw_set_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    _psram_send_direct_byte(0x99);  // Reset
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);

    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);

    // tRST = 5 µs minimum
    busy_wait_us(10);
}

static void _psram_enter_quad_mode(void) {
    // Send Quad Enable command (35h) in SPI mode
    hw_set_bits(&qmi_hw->direct_csr,
                QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    _psram_send_direct_byte(0x35);  // Enter Quad Mode
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    hw_clear_bits(&qmi_hw->direct_csr, QMI_DIRECT_CSR_EN_BITS);
    busy_wait_us(1);
}

void psram_init(void) {
    _psram_reset();
    _psram_enter_quad_mode();
    _psram_set_qspi_timing();

    // Invalidate the XIP cache so any stale lines covering the PSRAM
    // window (CS1) are dropped.  On RP2350 the cache-maintenance path
    // replaces the RP2040 xip_ctrl_hw->flush register.
    xip_cache_invalidate_all();
}
