#pragma once

#include <stdint.h>

enum {
    OMEGA_DDF_FULL_WIDTH_LIMIT = 0x40,
    OMEGA_DDF_EXTRA_WORD_START = 0x3c,
    OMEGA_DDF_EXTRA_WORD_STOP = 0xd0,
    OMEGA_DDF_ROTATED_START = 0x38,
    OMEGA_DDF_ROTATED_STOP = 0xd8,
    OMEGA_DDF_ROTATED_PIXELS = 80,
    OMEGA_DDF_NORMAL_FETCH_TAIL = 7,
    OMEGA_DDF_EXTRA_FETCH_TAIL = 11,
    OMEGA_DDF_LORES_FETCH_SPAN = 159,
    OMEGA_DDF_NORMAL_UPPER_OVERSCAN = 14,
    OMEGA_DDF_EXTRA_UPPER_OVERSCAN = 40,
    OMEGA_DISPLAY_RASTER_ORIGIN = 43,
    OMEGA_LORES_FIRST_RENDER_LINE = 44,
};

static inline int omegaDdfIsFullWidth(uint16_t start) {
    return start < OMEGA_DDF_FULL_WIDTH_LIMIT;
}

static inline int omegaDiwVerticalStart(uint16_t diwstrt) {
    return diwstrt >> 8;
}

static inline int omegaDiwVerticalStop(uint16_t diwstop) {
    return 0x100 | (diwstop >> 8);
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

static inline int omegaDdfHiresFetchTail(uint16_t start, uint16_t stop) {
    return omegaDdfNeedsExtraWord(start, stop)
         ? OMEGA_DDF_EXTRA_FETCH_TAIL : OMEGA_DDF_NORMAL_FETCH_TAIL;
}

static inline int omegaDdfUpperOverscan(uint16_t stop) {
    return stop == OMEGA_DDF_EXTRA_WORD_STOP
         ? OMEGA_DDF_EXTRA_UPPER_OVERSCAN
         : OMEGA_DDF_NORMAL_UPPER_OVERSCAN;
}
