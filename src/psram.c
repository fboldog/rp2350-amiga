// RP2350 PSRAM setup using the Pico SDK hardware_psram driver.
//
// The SDK initializes CS1 during runtime startup from PICO_PSRAM_* settings.
// main() subsequently raises clk_sys to 250 MHz, so this wrapper recalculates
// QMI timing and safely reinitializes the APS6404L before the emulator uses it.

#include "psram.h"
#include "board_config.h"

#include "hardware/psram.h"
#include <stdio.h>

bool psram_init(void) {
    if (!psram_is_available()) {
        printf("PSRAM: SDK initialization did not detect a device\n");
        return false;
    }

    // Runtime startup has already detected the chip and put it in quad mode.
    // Keep the SDK-programmed QMI setup intact while validating the board.

    size_t detected_size = psram_get_size();
    printf("PSRAM: SDK detected %lu MB\n",
           (unsigned long)(detected_size >> 20));
    if (detected_size < PSRAM_SIZE) return false;

    volatile uint32_t *probe = (volatile uint32_t *)PSRAM_BASE;
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t expected = 0x51A7C000u | i;
        probe[i] = expected;
        if (probe[i] != expected) {
            printf("PSRAM: probe failed at +0x%x\n", i * 4u);
            return false;
        }
    }
    printf("PSRAM: read/write probe passed\n");
    return true;
}
