// Minimal PSRAM validation built on Pico SDK hardware_psram.
//
// Linking hardware_psram makes the SDK detect and configure QMI PSRAM before
// main() runs. This file deliberately does not touch QMI registers; it mirrors
// the known-good rp2350b-psram test project's availability and byte-pattern
// checks.

#include "psram.h"

#include "hardware/psram.h"
#include "pico/stdlib.h"

#include <stddef.h>
#include <stdio.h>

#define PSRAM_TEST_SIZE 1024u

// Reserve the probe through the SDK's PSRAM linker region. The emulator later
// owns the complete fixed PSRAM map, so this startup-only buffer may safely
// overlap the beginning of chip RAM after psram_init() returns.
static volatile uint8_t __uninitialized_psram("omega_probe")
    psram_test_buffer[PSRAM_TEST_SIZE];

static uint8_t test_pattern(size_t index) {
    return (uint8_t)((index * 37u + 0x5au) & 0xffu);
}

bool psram_init(void) {
    if (!psram_is_available()) {
        printf("PSRAM: SDK did not detect or initialize a device\n");
        return false;
    }

    const size_t detected_size = psram_get_size();
    printf("PSRAM: detected %lu bytes on GPIO%u\n",
           (unsigned long)detected_size, (unsigned)PICO_PSRAM_CS_PIN);
    if (detected_size < PSRAM_SIZE) {
        printf("PSRAM: need at least %u bytes\n", (unsigned)PSRAM_SIZE);
        return false;
    }

    for (size_t i = 0; i < PSRAM_TEST_SIZE; ++i) {
        psram_test_buffer[i] = test_pattern(i);
    }

    for (size_t i = 0; i < PSRAM_TEST_SIZE; ++i) {
        const uint8_t expected = test_pattern(i);
        const uint8_t actual = psram_test_buffer[i];
        if (actual != expected) {
            printf("PSRAM: test failed at byte %u (expected %02x, read %02x)\n",
                   (unsigned)i, expected, actual);
            return false;
        }
    }

    printf("PSRAM: %u-byte read/write test passed at %p\n",
           (unsigned)PSRAM_TEST_SIZE, (void *)psram_test_buffer);
    return true;
}
