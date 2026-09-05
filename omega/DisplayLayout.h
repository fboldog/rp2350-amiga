#pragma once

#include <stdint.h>

enum {
    OMEGA_DDF_FULL_WIDTH_LIMIT = 0x40,
    OMEGA_DDF_EXTRA_WORD_START = 0x3c,
    OMEGA_DDF_EXTRA_WORD_STOP = 0xd0,
    OMEGA_DDF_ROTATED_START = 0x38,
    OMEGA_DDF_ROTATED_STOP = 0xd8,
    OMEGA_DDF_ROTATED_PIXELS = 80,
};

static inline int omegaDdfIsFullWidth(uint16_t start) {
    return start < OMEGA_DDF_FULL_WIDTH_LIMIT;
}

static inline int omegaDdfNeedsExtraWord(uint16_t start, uint16_t stop) {
    return start == OMEGA_DDF_EXTRA_WORD_START &&
           stop == OMEGA_DDF_EXTRA_WORD_STOP;
}

static inline int omegaDdfRowRotation(uint16_t start, uint16_t stop) {
    return start == OMEGA_DDF_ROTATED_START &&
           stop == OMEGA_DDF_ROTATED_STOP
         ? OMEGA_DDF_ROTATED_PIXELS : 0;
}
