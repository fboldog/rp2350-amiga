//
//  Audio.c
//  Paula audio: four DMA channels, mixed to 48 kHz stereo.
//
//  Each channel plays signed 8-bit samples, high byte of each word first,
//  one sample every AUDxPER colour clocks (DMA slots in this emulator), at
//  AUDxVOL (0-64). With DMA on, starting the channel copies AUDxLC/AUDxLEN
//  into its counters and raises the channel's interrupt, so software can
//  queue the next block; words are fetched as needed, and when the length
//  runs out the registers are reloaded and the interrupt raised again. With
//  DMA off, a CPU write to AUDxDAT plays that word, and the interrupt asks
//  for the next one. Attach modes (ADKCON) are not emulated.
//
//  The channels run in emulated time and are advanced once per raster line;
//  output samples fall at 48 kHz of emulated time, so the pitch is exact
//  relative to the emulated machine whatever the emulation speed.
//

#include "Audio.h"
#include "Chipset.h"
#include "VideoStandard.h"
#include "CIA.h"
#include <math.h>

// Mixing, filtering and hostAudioOut() only when something plays the
// output (RP2350: HDMI audio builds; the native runner always). Without
// it Paula still runs (DMA, interrupts), unmixed.
#ifndef OMEGA_AUDIO_OUTPUT
#define OMEGA_AUDIO_OUTPUT 1
#endif

#define SLOTS_PER_LINE 228   // hPos 0..0xE3
#define MIN_DMA_PERIOD 124   // shortest period audio DMA can feed

typedef struct {
    int mode;           // 0 off, 1 DMA, 2 CPU-written AUDxDAT
    uint32_t ptr;       // next word to fetch (word index into chip RAM)
    uint32_t len;       // words left in the current block
    uint16_t word;      // word being played
    int lowNext;        // its low byte is the next sample
    int64_t counter;    // colour clocks left of the current sample (16.16)
    int8_t sample;      // current output sample
    int datPending;     // CPU wrote AUDxDAT since the last word was taken
} AudioChannel;

static AudioChannel ch[4];
// Sum of sample x colour clocks (16.16) of each channel over the current
// output interval: each output sample is the average over its interval (a
// box filter), not a point sample, which keeps aliasing down.
static int32_t accum[4];
static int active;      // channels with mode != 0 (bit per channel)
// Colour clocks per output sample and until the next one (16.16).
static const uint32_t outStep = (uint32_t)(
    ((uint64_t)SLOTS_PER_LINE * OMEGA_VIDEO_FRAME_LINES *
     OMEGA_VIDEO_RATE_NUMERATOR << 16) /
    ((uint64_t)OMEGA_VIDEO_RATE_DENOMINATOR * AUDIO_RATE));
static int32_t outNext;
// accum (sample x clocks in 16.12) x volume -> fraction of full scale for
// one output interval; two channels per side, so each counts half.
static float outScale;

static inline uint32_t regLc(int c) {
    return c == 0 ? chipset.aud0lc : c == 1 ? chipset.aud1lc :
           c == 2 ? chipset.aud2lc : chipset.aud3lc;
}
static inline uint16_t regLen(int c) {
    return c == 0 ? chipset.aud0len : c == 1 ? chipset.aud1len :
           c == 2 ? chipset.aud2len : chipset.aud3len;
}
static inline uint16_t regPer(int c) {
    return c == 0 ? chipset.aud0per : c == 1 ? chipset.aud1per :
           c == 2 ? chipset.aud2per : chipset.aud3per;
}
static inline uint16_t regVol(int c) {
    return c == 0 ? chipset.aud0vol : c == 1 ? chipset.aud1vol :
           c == 2 ? chipset.aud2vol : chipset.aud3vol;
}
static inline uint16_t regDat(int c) {
    return c == 0 ? chipset.aud0dat : c == 1 ? chipset.aud1dat :
           c == 2 ? chipset.aud2dat : chipset.aud3dat;
}

static void raiseInterrupt(int c) {
    putChipReg16[INTREQ]((uint16_t)(0x8000u | (0x80u << c)));
}

static void reload(int c) {
    ch[c].ptr = regLc(c);
    ch[c].len = regLen(c) ? regLen(c) : 0x10000u;
    raiseInterrupt(c);
}

static int dmaOn(int c) {
    return (chipset.dmaconr & (0x200u | (1u << c))) == (0x200u | (1u << c));
}

// The Amiga's analogue output stage, per side: a fixed first-order low-pass
// (~4.9 kHz on the A500) and the "LED" filter, a second-order Butterworth
// low-pass at ~3.3 kHz switched on while CIA-A's /LED output (PRA bit 1)
// is low.
typedef struct { float y; } OnePole;
typedef struct { float x1, x2, y1, y2; } Biquad;
static float fixedA;                              // one-pole coefficient
static float ledB0, ledB1, ledB2, ledA1, ledA2;   // biquad coefficients
static OnePole fixedL, fixedR;
static Biquad ledL, ledR;

static void filtersInit(void) {
    const float pi = 3.14159265f;
    fixedA = 1.0f - expf(-2.0f * pi * 4900.0f / AUDIO_RATE);
    // RBJ cookbook low-pass, Q = 1/sqrt(2).
    const float w = 2.0f * pi * 3275.0f / AUDIO_RATE;
    const float alpha = sinf(w) / (2.0f * 0.70710678f);
    const float c = cosf(w), a0 = 1.0f + alpha;
    ledB0 = (1.0f - c) / 2.0f / a0;
    ledB1 = (1.0f - c) / a0;
    ledB2 = ledB0;
    ledA1 = -2.0f * c / a0;
    ledA2 = (1.0f - alpha) / a0;
}

static void setMode(int c, int mode) {
    ch[c].mode = mode;
    if (mode)
        active |= 1 << c;
    else
        active &= ~(1 << c);
}

void audioReset(void) {
    if (outScale == 0.0f) {
        outScale = 1.0f / ((float)(outStep >> 4) * 128.0f * 64.0f) * 0.5f;
        filtersInit();
    }
    fixedL.y = fixedR.y = 0.0f;
    ledL = ledR = (Biquad){0};
    for (int c = 0; c < 4; ++c)
        accum[c] = 0;
    for (int c = 0; c < 4; ++c)
        ch[c] = (AudioChannel){0};
    active = 0;
    outNext = 0;
}

void audioDmaconChanged(uint16_t old) {
    for (int c = 0; c < 4; ++c) {
        const int was = (old & (0x200u | (1u << c))) == (0x200u | (1u << c));
        const int now = dmaOn(c);
        if (now && !was) {
            setMode(c, 1);
            ch[c].lowNext = 0;
            ch[c].counter = 0;
            reload(c);
        } else if (!now && was && ch[c].mode == 1) {
            setMode(c, 0);
            ch[c].sample = 0;
        }
    }
}

void audioDatWritten(int c) {
    if (dmaOn(c))
        return;  // DMA owns the channel
    ch[c].datPending = 1;
    if (ch[c].mode == 0) {
        setMode(c, 2);
        ch[c].lowNext = 0;
        ch[c].counter = 0;
    }
}

// Moves channel c to its next sample.
static void nextSample(int c) {
    AudioChannel *a = &ch[c];
    if (a->lowNext) {
        a->sample = (int8_t)(a->word & 0xFF);
        a->lowNext = 0;
        return;
    }
    if (a->mode == 1) {
        // chip RAM words are stored host-endian: swap to the Amiga's order
        const uint16_t w = internal.chipramW[a->ptr & 0xFFFFFu];
        a->word = (uint16_t)(w << 8 | w >> 8);
        a->ptr += 1;
        if (--a->len == 0)
            reload(c);
    } else {
        // CPU playback: a written word plays once; the interrupt asks for
        // the next, and without one the channel goes idle.
        if (!a->datPending) {
            setMode(c, 0);
            a->sample = 0;
            return;
        }
        a->datPending = 0;
        a->word = regDat(c);
        raiseInterrupt(c);
    }
    a->sample = (int8_t)(a->word >> 8);
    a->lowNext = 1;
}

static void advance(int c, int32_t clocks) {
    AudioChannel *a = &ch[c];
    while (clocks > 0 && a->mode != 0) {
        const int32_t seg = a->counter < clocks ? (int32_t)a->counter : clocks;
        accum[c] += a->sample * (seg >> 4);   // 16.12: fits in 32 bits
        a->counter -= seg;
        clocks -= seg;
        if (a->counter <= 0) {
            nextSample(c);
            uint32_t per = regPer(c);
            if (per == 0)
                per = 0x10000;  // the period counter wraps: 0 is the longest
            if (a->mode == 1 && per < MIN_DMA_PERIOD)
                per = MIN_DMA_PERIOD;
            a->counter += (int64_t)per << 16;
        }
    }
}

// Channel c's average over the output interval, times its volume, as a
// fraction of full scale (+-1 for sample +-128 at volume 64).
static float channelOut(int c) {
    const int32_t sum = accum[c];
    accum[c] = 0;
    uint32_t vol = regVol(c) & 0x7F;
    if (vol > 64)
        vol = 64;
    return (float)sum * (float)vol * outScale;
}

static float filter(float x, OnePole *f, Biquad *b, int led) {
    f->y += (x - f->y) * fixedA;
    x = f->y;
    if (!led)
        return x;
    const float y = ledB0 * x + ledB1 * b->x1 + ledB2 * b->x2 -
                    ledA1 * b->y1 - ledA2 * b->y2;
    b->x2 = b->x1; b->x1 = x;
    b->y2 = b->y1; b->y1 = y;
    return y;
}

static int16_t toS16(float v) {
    const float s = v * 32767.0f;
    return (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : (int)s);
}


void audioLine(void) {
    const int32_t lineClocks = SLOTS_PER_LINE << 16;
#if !OMEGA_AUDIO_OUTPUT
    // No output: advance the channels (DMA fetches, interrupts) only.
    if (active)
        for (int c = 0; c < 4; ++c) {
            advance(c, lineClocks);
            accum[c] = 0;
        }
    return;
#endif
    if (!active && fixedL.y == 0.0f && fixedR.y == 0.0f) {
        // All channels off and the filters settled (most of the time): only
        // count the samples.
        int frames = 0;
        while (outNext < lineClocks) {
            ++frames;
            outNext += (int32_t)outStep;
        }
        outNext -= lineClocks;
        hostAudioOut(0, frames);
        return;
    }
    int16_t out[2 * 4];  // a line holds at most 4 output samples
    const int led = !(CIAA.pra & 0x02);
    int frames = 0;
    int32_t pos = 0;
    while (outNext < lineClocks) {
        const int32_t d = outNext - pos;
        for (int c = 0; c < 4; ++c)
            advance(c, d);
        pos = outNext;
        const float l = channelOut(0) + channelOut(3);
        const float r = channelOut(1) + channelOut(2);
        float fl = filter(l, &fixedL, &ledL, led);
        float fr = filter(r, &fixedR, &ledR, led);
        // Let a decayed filter reach exact silence (enables the fast path).
        if (!active && fabsf(fixedL.y) < 1e-5f && fabsf(fixedR.y) < 1e-5f) {
            fixedL.y = fixedR.y = 0.0f;
            fl = fr = 0.0f;
        }
        out[2 * frames] = toS16(fl);
        out[2 * frames + 1] = toS16(fr);
        ++frames;
        outNext += (int32_t)outStep;
    }
    for (int c = 0; c < 4; ++c)
        advance(c, lineClocks - pos);
    outNext -= lineClocks;
    hostAudioOut(out, frames);
}
